//
// Created by Claude on 2026/4/19.
// RKNN 检测器实现
//

#include "rknn_detector.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <sys/mman.h>
#include <cfloat>
#include "log.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_drm.h>
#include <libswscale/swscale.h>
}

namespace nvr::detection {

// ============================================================================
// RKNNDetector 实现
// ============================================================================

RKNNDetector::RKNNDetector(const DetectionConfig& config)
    : config_(config)
    , rknn_ctx_(0)
    , initialized_(false)
    , logger_(spdlog::get("nvr") ? spdlog::get("nvr") : spdlog::default_logger())
{
    std::memset(inputs_, 0, sizeof(inputs_));
}

RKNNDetector::~RKNNDetector() {
    shutdown();
}

bool RKNNDetector::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        LOG_WARN("RKNNDetector already initialized");
        return true;
    }

    LOG_INFO("Initializing RKNNDetector with model: {}", config_.model_path);

    // 加载模型
    if (!loadModel()) {
        LOG_ERROR("Failed to load RKNN model");
        return false;
    }

    // 初始化 RKNN 上下文
    if (!initRKNNContext()) {
        LOG_ERROR("Failed to initialize RKNN context");
        releaseResources();
        return false;
    }

    // 查询模型信息
    if (!queryModelInfo()) {
        LOG_ERROR("Failed to query model info");
        releaseResources();
        return false;
    }

    // 设置输入输出
    if (!setupInputsOutputs()) {
        LOG_ERROR("Failed to setup inputs/outputs");
        releaseResources();
        return false;
    }

    initialized_ = true;
    LOG_INFO("RKNNDetector initialized successfully");
    LOG_INFO("Model input: {}x{}x{}", model_info_.input_width, model_info_.input_height, model_info_.input_channels);

    // 零拷贝模式：设置持久化 IO 内存和 RGA 预处理器
#ifdef ENABLE_RKNN_SMART_RECORDING
    if (config_.zero_copy_enabled) {
        // 设置零拷贝 IO
        if (!setupZeroCopyIO()) {
            LOG_ERROR("Failed to setup zero-copy IO, falling back to non-zero-copy mode");
            config_.zero_copy_enabled = false;
        } else {
            // 初始化 RGA 预处理器（使用持久化的 RKNN 输入内存 fd）
            int dst_wstride = native_input_attrs_[0].w_stride;
            if (dst_wstride == 0) dst_wstride = model_info_.input_width;

            rga_preprocessor_ = std::make_unique<RGAPreprocessor>();
            if (!rga_preprocessor_->initialize(input_mem_->fd, model_info_.input_width,
                                                    model_info_.input_height, dst_wstride)) {
                LOG_WARN("RGA preprocessor init failed, will use CPU fallback");
                rga_preprocessor_.reset();
            }
        }
    }
#endif

    return true;
}

void RKNNDetector::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return;
    }

    LOG_INFO("Shutting down RKNNDetector");
    releaseResources();
    initialized_ = false;
}

bool RKNNDetector::setCoreMask(uint32_t core_mask) {
    if (!initialized_) {
        LOG_ERROR("Cannot set core mask: detector not initialized");
        return false;
    }
    int ret = rknn_set_core_mask(rknn_ctx_, static_cast<rknn_core_mask>(core_mask));
    if (ret != RKNN_SUCC) {
        LOG_ERROR("rknn_set_core_mask failed: {}", ret);
        return false;
    }
    LOG_INFO("Set NPU core mask to {}", core_mask);
    return true;
}

bool RKNNDetector::loadModel() {
    std::ifstream file(config_.model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open model file: {}", config_.model_path);
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    model_data_.resize(size);
    if (!file.read(reinterpret_cast<char*>(model_data_.data()), size)) {
        LOG_ERROR("Failed to read model file");
        return false;
    }

    LOG_INFO("Loaded RKNN model, size: {} bytes", size);
    return true;
}

bool RKNNDetector::initRKNNContext() {
    int ret = rknn_init(&rknn_ctx_, model_data_.data(), model_data_.size(), 0, nullptr);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("rknn_init failed: {}", ret);
        return false;
    }

    LOG_DEBUG("RKNN context initialized");
    return true;
}

bool RKNNDetector::queryModelInfo() {
    rknn_input_output_num io_num_attr;
    int ret = rknn_query(rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_attr, sizeof(io_num_attr));
    if (ret < 0) {
        LOG_ERROR("rknn_query RKNN_QUERY_IN_OUT_NUM failed: {}", ret);
        return false;
    }

    LOG_DEBUG("Model input num: {}, output num: {}", io_num_attr.n_input, io_num_attr.n_output);

    // 查询输入属性
    rknn_tensor_attr input_attr;
    input_attr.index = 0;
    ret = rknn_query(rknn_ctx_, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    if (ret < 0) {
        LOG_ERROR("rknn_query RKNN_QUERY_INPUT_ATTR failed: {}", ret);
        return false;
    }

    model_info_.input_width = input_attr.dims[1];
    model_info_.input_height = input_attr.dims[2];
    model_info_.input_channels = input_attr.dims[3];
    model_info_.num_outputs = io_num_attr.n_output;

    LOG_DEBUG("Input: {}x{}x{}", model_info_.input_width, model_info_.input_height, model_info_.input_channels);

    // 查询输出属性（用户面 NCHW 逻辑尺寸）
    model_info_.output_sizes.clear();
    output_attrs_.resize(io_num_attr.n_output);
    for (uint32_t i = 0; i < io_num_attr.n_output; i++) {
        rknn_tensor_attr output_attr;
        output_attr.index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attr, sizeof(output_attr));
        if (ret < 0) {
            LOG_ERROR("rknn_query RKNN_QUERY_OUTPUT_ATTR failed for output {}: {}", i, ret);
            return false;
        }
        model_info_.output_sizes.push_back(output_attr.n_elems);
        output_attrs_[i] = output_attr;
        LOG_DEBUG("Output {}: size = {}, dims = [{},{},{},{}]",
                      i, output_attr.n_elems,
                      output_attr.dims[0], output_attr.dims[1],
                      output_attr.dims[2], output_attr.dims[3]);
    }

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 查询 NATIVE 输入属性（NC1HWC2 格式，含 stride）
    native_input_attrs_.resize(io_num_attr.n_input);
    for (uint32_t i = 0; i < io_num_attr.n_input; i++) {
        native_input_attrs_[i].index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_NATIVE_INPUT_ATTR, &native_input_attrs_[i], sizeof(native_input_attrs_[i]));
        if (ret < 0) {
            LOG_ERROR("rknn_query RKNN_QUERY_NATIVE_INPUT_ATTR failed: {}", ret);
            return false;
        }
        LOG_INFO("Native input[{}]: w_stride={}, size_with_stride={}, fmt={}",
                     i, native_input_attrs_[i].w_stride, native_input_attrs_[i].size_with_stride,
                     static_cast<int>(native_input_attrs_[i].fmt));
    }

    // 查询 NATIVE 输出属性（NC1HWC2 int8 格式）
    native_output_attrs_.resize(io_num_attr.n_output);
    for (uint32_t i = 0; i < io_num_attr.n_output; i++) {
        native_output_attrs_[i].index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &native_output_attrs_[i], sizeof(native_output_attrs_[i]));
        if (ret < 0) {
            LOG_ERROR("rknn_query RKNN_QUERY_NATIVE_OUTPUT_ATTR failed: {}", ret);
            return false;
        }
        LOG_INFO("Native output[{}]: size_with_stride={}, fmt={}, zp={}, scale={:.6f}",
                     i, native_output_attrs_[i].size_with_stride, static_cast<int>(native_output_attrs_[i].fmt),
                     native_output_attrs_[i].zp, native_output_attrs_[i].scale);
    }

    // 判断是否为量化模型
    is_quant_ = (native_output_attrs_[0].fmt == RKNN_TENSOR_NC1HWC2 &&
                  native_output_attrs_[0].type == RKNN_TENSOR_INT8);
    LOG_INFO("Model is {}quantized", is_quant_ ? "" : "not ");
#endif

    return true;
}

bool RKNNDetector::setupInputsOutputs() {
    // 设置输入
    inputs_[0].index = 0;
    inputs_[0].type = RKNN_TENSOR_UINT8;
    inputs_[0].size = model_info_.input_width * model_info_.input_height * model_info_.input_channels;
    inputs_[0].fmt = RKNN_TENSOR_NHWC;
    inputs_[0].pass_through = 0;

    // 设置输出（数量与模型实际输出数一致）
    outputs_.resize(model_info_.num_outputs);
    for (int i = 0; i < model_info_.num_outputs; i++) {
        outputs_[i].index = i;
        outputs_[i].want_float = 1;
        outputs_[i].is_prealloc = 0;
    }

    LOG_DEBUG("Inputs/outputs setup completed ({} outputs)", model_info_.num_outputs);
    return true;
}

void RKNNDetector::releaseResources() {
    // 先释放 RGA 预处理器（会释放持久化 RGA 句柄）
    rga_preprocessor_.reset();

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 释放零拷贝内存
    releaseZeroCopyMem();
#endif

    // 销毁 RKNN 上下文
    if (rknn_ctx_ != 0) {
        rknn_destroy(rknn_ctx_);
        rknn_ctx_ = 0;
    }

    model_data_.clear();
    outputs_.clear();
    LOG_DEBUG("Resources released");
}

bool RKNNDetector::detectFrame(AVFrame* frame, DetectionResult& result) {
    if (!initialized_) {
        LOG_ERROR("RKNNDetector not initialized");
        return false;
    }

    if (!frame) {
        LOG_ERROR("Invalid frame");
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // 预处理帧
    std::vector<uint8_t> input_data;
    if (!preprocessFrame(frame, input_data)) {
        LOG_ERROR("Failed to preprocess frame");
        return false;
    }

    // 设置输入
    inputs_[0].buf = input_data.data();
    int ret = rknn_inputs_set(rknn_ctx_, 1, inputs_);
    if (ret < 0) {
        LOG_ERROR("rknn_inputs_set failed: {}", ret);
        return false;
    }

    if (config_.dump_detect.enable) {
        last_input_rgb_.assign(input_data.begin(), input_data.end());
        last_input_w_ = model_info_.input_width;
        last_input_h_ = model_info_.input_height;
    }

    // 执行推理
    ret = rknn_run(rknn_ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn_run failed: {}", ret);
        return false;
    }

    // 获取输出
    ret = rknn_outputs_get(rknn_ctx_, outputs_.size(), outputs_.data(), nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn_outputs_get failed: {}", ret);
        return false;
    }

    // 解析检测结果
    if (!parseDetectionOutputs(outputs_.data(), result)) {
        LOG_ERROR("Failed to parse detection outputs");
        rknn_outputs_release(rknn_ctx_, outputs_.size(), outputs_.data());
        return false;
    }

    // 释放输出
    rknn_outputs_release(rknn_ctx_, outputs_.size(), outputs_.data());

    // 计算处理时间
    auto end_time = std::chrono::high_resolution_clock::now();
    result.processing_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    // 更新统计信息
    stats_.update(result);

    return true;
}

bool RKNNDetector::detectFrameZeroCopy(const DMABufferInfo& dma_info, DetectionResult& result) {
    if (!initialized_) {
        LOG_ERROR("RKNNDetector not initialized");
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // 1. RGA 硬件预处理：DMA fd → RGB 直接写入持久化 RKNN 输入内存
    bool rga_ok = false;
    if (rga_preprocessor_ && rga_preprocessor_->isInitialized()) {
        rga_ok = rga_preprocessor_->resizeNV12toRGB(
            dma_info.fd, dma_info.width, dma_info.height,
            dma_info.format, dma_info.stride, dma_info.height_stride);
    }

    if (!rga_ok) {
        // 2. CPU fallback：直接写入 RKNN 输入内存
        LOG_DEBUG("RGA preprocess failed, using CPU fallback");
        cpuFallbackNV12toRGB(dma_info);
    }

    if (config_.dump_detect.enable) {
        // 保存调试图像（从持久化输入内存拷贝实际 RGB 数据）
        int image_size = model_info_.input_width * model_info_.input_height * model_info_.input_channels;
        last_input_rgb_.assign(static_cast<const uint8_t*>(input_mem_->virt_addr),
                               static_cast<const uint8_t*>(input_mem_->virt_addr) + image_size);
        last_input_w_ = model_info_.input_width;
        last_input_h_ = model_info_.input_height;
    }

    // 3. RKNN 推理（无需 rknn_inputs_set，已通过 rknn_set_io_mem 绑定）
    int ret = rknn_run(rknn_ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("rknn_run failed: {}", ret);
        return false;
    }

    // 4. 从持久化输出内存解析检测结果（NC1HWC2 → NCHW 反量化）
    if (!parseDetectionOutputsZeroCopy(result)) {
        LOG_ERROR("Failed to parse detection outputs");
        return false;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    result.processing_time_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    result.frame_pts = 0;

    stats_.update(result);
    return true;
}

bool RKNNDetector::preprocessFrame(AVFrame* frame, std::vector<uint8_t>& output_data) {
    if (!frame) {
        return false;
    }

    int src_width = frame->width;
    int src_height = frame->height;
    AVPixelFormat src_format = static_cast<AVPixelFormat>(frame->format);

    int dst_width = model_info_.input_width;
    int dst_height = model_info_.input_height;
    AVPixelFormat dst_format = AV_PIX_FMT_RGB24;

    // 创建 SwsContext
    SwsContext* sws_ctx = sws_getContext(
        src_width, src_height, src_format,
        dst_width, dst_height, dst_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx) {
        LOG_ERROR("Failed to create SwsContext");
        return false;
    }

    // 分配输出帧
    AVFrame* dst_frame = av_frame_alloc();
    dst_frame->width = dst_width;
    dst_frame->height = dst_height;
    dst_frame->format = dst_format;
    av_frame_get_buffer(dst_frame, 0);

    // 调整大小并转换格式
    sws_scale(sws_ctx,
              frame->data, frame->linesize, 0, src_height,
              dst_frame->data, dst_frame->linesize);

    // 复制数据到输出向量
    output_data.resize(dst_width * dst_height * 3);
    memcpy(output_data.data(), dst_frame->data[0], output_data.size());

    // 清理
    av_frame_free(&dst_frame);
    sws_freeContext(sws_ctx);

    return true;
}

bool RKNNDetector::parseDetectionOutputs(rknn_output* outputs, DetectionResult& result) {
    if (!outputs || !outputs[0].buf) {
        LOG_ERROR("Invalid outputs");
        return false;
    }

    result.player_class_id = config_.player_class_id;
    result.npc_class_id = config_.npc_class_id;

    std::vector<BoundingBox> all_boxes;

    if (model_info_.num_outputs <= 3) {
        // 单输出格式: (1, 4+nc, 8400)，cxcywh 坐标
        // 参考 rknn_model_zoo/examples/yolo11/python/t.py post_process
        float* data = static_cast<float*>(outputs[0].buf);
        int channels = output_attrs_[0].dims[1];       // 4 + num_classes
        int total_preds = output_attrs_[0].n_elems / channels;  // 8400
        int num_classes = channels - 4;

        for (int i = 0; i < total_preds; i++) {
            // 找最大 class score
            float max_score = 0;
            int max_cls_id = -1;
            for (int c = 0; c < num_classes; c++) {
                float score = data[(4 + c) * total_preds + i];
                if (score > max_score) {
                    max_score = score;
                    max_cls_id = c;
                }
            }

            if (max_score < config_.confidence_threshold) {
                continue;
            }

            // cxcywh → xyxy
            float cx = data[0 * total_preds + i];
            float cy = data[1 * total_preds + i];
            float w  = data[2 * total_preds + i];
            float h  = data[3 * total_preds + i];
            float x1 = cx - w / 2.0f;
            float y1 = cy - h / 2.0f;

            if (w > 0 && h > 0) {
                all_boxes.emplace_back(x1, y1, w, h, max_score, max_cls_id);
            }
        }
    } else {
        // DFL 多分支格式（9 个输出，3 个检测头 × 3 输出）:
        //   [3*i]:   DFL bbox  [dfl_len*4, grid_h, grid_w]
        //   [3*i+1]: class scores [num_classes, grid_h, grid_w]
        //   [3*i+2]: score sum [1, grid_h, grid_w] (快速过滤)
        const int num_classes = model_info_.output_sizes[1] / model_info_.output_sizes[2];
        const int reg_max = model_info_.output_sizes[0] / (model_info_.output_sizes[2]) / 4;

        for (int s = 0; s < 3; s++) {
            if (3 * s + 2 >= static_cast<int>(model_info_.output_sizes.size())) break;

            float* bbox_dfl = static_cast<float*>(outputs[3 * s].buf);
            float* cls_scores = static_cast<float*>(outputs[3 * s + 1].buf);
            float* score_sum = static_cast<float*>(outputs[3 * s + 2].buf);

            int grid_len = model_info_.output_sizes[3 * s + 2];
            int grid = 1;
            while (grid * grid < grid_len) grid++;
            int grid_h = grid;
            int grid_w = grid;
            int stride = model_info_.input_height / grid_h;

            for (int i = 0; i < grid_h; i++) {
                for (int j = 0; j < grid_w; j++) {
                    int offset = i * grid_w + j;

                    if (score_sum[offset] < config_.confidence_threshold) {
                        continue;
                    }

                    float max_score = 0;
                    int max_cls_id = -1;
                    int cls_offset = offset;
                    for (int c = 0; c < num_classes; c++) {
                        if (cls_scores[cls_offset] > max_score) {
                            max_score = cls_scores[cls_offset];
                            max_cls_id = c;
                        }
                        cls_offset += grid_len;
                    }

                    if (max_score < config_.confidence_threshold) {
                        continue;
                    }

                    float box[4];
                    float before_dfl[reg_max * 4];
                    int dfl_offset = offset;
                    for (int k = 0; k < reg_max * 4; k++) {
                        before_dfl[k] = bbox_dfl[dfl_offset];
                        dfl_offset += grid_len;
                    }

                    for (int b = 0; b < 4; b++) {
                        float sum_exp = 0;
                        for (int r = 0; r < reg_max; r++) {
                            before_dfl[b * reg_max + r] = std::exp(before_dfl[b * reg_max + r]);
                            sum_exp += before_dfl[b * reg_max + r];
                        }
                        float acc = 0;
                        for (int r = 0; r < reg_max; r++) {
                            acc += (before_dfl[b * reg_max + r] / sum_exp) * r;
                        }
                        box[b] = acc;
                    }

                    float x1 = (-box[0] + j + 0.5f) * stride;
                    float y1 = (-box[1] + i + 0.5f) * stride;
                    float x2 = (box[2] + j + 0.5f) * stride;
                    float y2 = (box[3] + i + 0.5f) * stride;
                    float w = x2 - x1;
                    float h = y2 - y1;

                    if (w > 0 && h > 0) {
                        all_boxes.emplace_back(x1, y1, w, h, max_score, max_cls_id);
                    }
                }
            }
        }
    }

    // 应用 NMS
    result.boxes = applyNMS(all_boxes);

    for (const auto& box : result.boxes) {
        if (box.class_id == config_.player_class_id) {
            result.has_player = true;
            if (box.confidence > result.player_confidence) {
                result.player_confidence = box.confidence;
            }
        }
        if (box.class_id == config_.npc_class_id) {
            result.has_npc = true;
            if (box.confidence > result.npc_confidence) {
                result.npc_confidence = box.confidence;
            }
        }
    }

    return true;
}

void RKNNDetector::yolo11PostProcess(float* output_data, int output_size, DetectionResult& result) {
    // 已由 parseDetectionOutputs 处理
}

std::vector<BoundingBox> RKNNDetector::applyNMS(
    const std::vector<BoundingBox>& boxes,
    float iou_threshold) {

    if (boxes.empty()) {
        return {};
    }

    // 按置信度排序
    std::vector<BoundingBox> sorted_boxes = boxes;
    std::sort(sorted_boxes.begin(), sorted_boxes.end(),
              [](const BoundingBox& a, const BoundingBox& b) {
                  return a.confidence > b.confidence;
              });

    std::vector<BoundingBox> nms_boxes;
    std::vector<bool> suppressed(boxes.size(), false);

    for (size_t i = 0; i < sorted_boxes.size(); ++i) {
        if (suppressed[i]) {
            continue;
        }

        nms_boxes.push_back(sorted_boxes[i]);

        for (size_t j = i + 1; j < sorted_boxes.size(); ++j) {
            if (suppressed[j]) {
                continue;
            }

            // 如果是同一类别且 IoU 超过阈值，抑制
            if (sorted_boxes[i].class_id == sorted_boxes[j].class_id) {
                float iou = sorted_boxes[i].iou(sorted_boxes[j]);
                if (iou > iou_threshold) {
                    suppressed[j] = true;
                }
            }
        }
    }

    return nms_boxes;
}

bool RKNNDetector::warmup(int iterations) {
    if (!initialized_) {
        LOG_ERROR("Cannot warmup: detector not initialized");
        return false;
    }

    LOG_INFO("Warming up RKNN detector with {} iterations...", iterations);

#ifdef ENABLE_RKNN_SMART_RECORDING
    if (config_.zero_copy_enabled && input_mem_) {
        // 零拷贝预热：直接写入持久化输入内存
        int input_size = native_input_attrs_[0].size_with_stride;
        std::memset(input_mem_->virt_addr, 128, input_size);

        for (int i = 0; i < iterations; i++) {
            int ret = rknn_run(rknn_ctx_, nullptr);
            if (ret < 0) {
                LOG_ERROR("Warmup failed at iteration {}", i);
                return false;
            }
        }
    } else {
#endif
        // 非零拷贝预热：使用传统 API
        std::vector<uint8_t> dummy_input(
            model_info_.input_width * model_info_.input_height * model_info_.input_channels,
            128);

        for (int i = 0; i < iterations; i++) {
            inputs_[0].buf = dummy_input.data();

            int ret = rknn_inputs_set(rknn_ctx_, 1, inputs_);
            if (ret < 0) {
                LOG_ERROR("Warmup failed at iteration {}", i);
                return false;
            }

            ret = rknn_run(rknn_ctx_, nullptr);
            if (ret < 0) {
                LOG_ERROR("Warmup failed at iteration {}", i);
                return false;
            }

            ret = rknn_outputs_get(rknn_ctx_, outputs_.size(), outputs_.data(), nullptr);
            if (ret < 0) {
                LOG_ERROR("Warmup failed at iteration {}", i);
                return false;
            }

            rknn_outputs_release(rknn_ctx_, outputs_.size(), outputs_.data());
        }
#ifdef ENABLE_RKNN_SMART_RECORDING
    }
#endif

    LOG_INFO("Warmup completed successfully");
    return true;
}

#ifdef ENABLE_RKNN_SMART_RECORDING
// ============================================================================
// 零拷贝方法实现
// ============================================================================

bool RKNNDetector::setupZeroCopyIO() {
    // 覆盖输入类型为 UINT8（融合量化和反量化）
    native_input_attrs_[0].type = RKNN_TENSOR_UINT8;

    // 分配并绑定输入内存
    input_mem_ = rknn_create_mem(rknn_ctx_, native_input_attrs_[0].size_with_stride);
    if (!input_mem_) {
        LOG_ERROR("setupZeroCopyIO: rknn_create_mem(input) failed");
        return false;
    }

    int ret = rknn_set_io_mem(rknn_ctx_, input_mem_, &native_input_attrs_[0]);
    if (ret < 0) {
        LOG_ERROR("setupZeroCopyIO: rknn_set_io_mem(input) failed: {}", ret);
        rknn_destroy_mem(rknn_ctx_, input_mem_);
        input_mem_ = nullptr;
        return false;
    }

    LOG_INFO("RKNN input zero-copy: size={}, size_with_stride={}, w_stride={}",
                 native_input_attrs_[0].size, native_input_attrs_[0].size_with_stride,
                 native_input_attrs_[0].w_stride);

    // 分配并绑定输出内存
    output_mems_.resize(model_info_.num_outputs);
    float_outputs_.resize(model_info_.num_outputs);

    for (uint32_t i = 0; i < model_info_.num_outputs; ++i) {
        // 非量化模型：请求 RKNN 运行时将输出转为 float32 NCHW
        if (!is_quant_) {
            native_output_attrs_[i].type = RKNN_TENSOR_FLOAT32;
            native_output_attrs_[i].fmt = RKNN_TENSOR_NCHW;
            native_output_attrs_[i].size_with_stride = output_attrs_[i].n_elems * sizeof(float);
        }

        output_mems_[i] = rknn_create_mem(rknn_ctx_, native_output_attrs_[i].size_with_stride);
        if (!output_mems_[i]) {
            LOG_ERROR("setupZeroCopyIO: rknn_create_mem(output[{}]) failed", i);
            // 清理已分配的内存
            for (uint32_t j = 0; j < i; ++j) {
                rknn_destroy_mem(rknn_ctx_, output_mems_[j]);
            }
            rknn_destroy_mem(rknn_ctx_, input_mem_);
            input_mem_ = nullptr;
            output_mems_.clear();
            return false;
        }

        ret = rknn_set_io_mem(rknn_ctx_, output_mems_[i], &native_output_attrs_[i]);
        if (ret < 0) {
            LOG_ERROR("setupZeroCopyIO: rknn_set_io_mem(output[{}]) failed: {}", i, ret);
            // 清理
            for (uint32_t j = 0; j <= i; ++j) {
                rknn_destroy_mem(rknn_ctx_, output_mems_[j]);
            }
            rknn_destroy_mem(rknn_ctx_, input_mem_);
            input_mem_ = nullptr;
            output_mems_.clear();
            return false;
        }

        // 预分配 float 转换缓冲区
        int elem_count = output_attrs_[i].n_elems;
        float_outputs_[i].resize(elem_count);

        LOG_INFO("RKNN output[{}] zero-copy: size={}, size_with_stride={}, zp={}, scale={:.6f}",
                     i, native_output_attrs_[i].size, native_output_attrs_[i].size_with_stride,
                     native_output_attrs_[i].zp, native_output_attrs_[i].scale);
    }

    LOG_INFO("RKNN zero-copy IO setup completed");
    return true;
}

void RKNNDetector::releaseZeroCopyMem() {
    if (input_mem_) {
        rknn_destroy_mem(rknn_ctx_, input_mem_);
        input_mem_ = nullptr;
    }

    for (auto& mem : output_mems_) {
        if (mem) {
            rknn_destroy_mem(rknn_ctx_, mem);
            mem = nullptr;
        }
    }
    output_mems_.clear();
    float_outputs_.clear();
}

void RKNNDetector::convertNC1HWC2ToFloat(int output_idx, std::vector<float>& out_buf) {
    auto& native_attr = native_output_attrs_[output_idx];
    auto& user_attr = output_attrs_[output_idx];

    int8_t* src = static_cast<int8_t*>(output_mems_[output_idx]->virt_addr);
    int zp = native_attr.zp;
    float scale = native_attr.scale;

    // NCHW 维度
    int channel = user_attr.dims[1];
    int h = user_attr.n_dims > 2 ? user_attr.dims[2] : 1;
    int w = user_attr.n_dims > 3 ? user_attr.dims[3] : 1;

    // NC1HWC2 维度
    int C1 = native_attr.dims[1];
    int C2 = native_attr.dims[4];
    int H = native_attr.dims[2];
    int W = native_attr.dims[3];

    out_buf.resize(channel * h * w);

    // NC1HWC2 → NCHW 转换 + 反量化
    for (int c = 0; c < channel; ++c) {
        int plane = c / C2;
        int offset = c % C2;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int src_idx = (plane * H * W + y * W + x) * C2 + offset;
                int dst_idx = c * H * W + y * W + x;
                out_buf[dst_idx] = (static_cast<float>(src[src_idx]) - zp) * scale;
            }
        }
    }
}

bool RKNNDetector::parseDetectionOutputsZeroCopy(DetectionResult& result) {
    for (uint32_t i = 0; i < model_info_.num_outputs; ++i) {
        if (is_quant_ && native_output_attrs_[i].fmt == RKNN_TENSOR_NC1HWC2) {
            convertNC1HWC2ToFloat(i, float_outputs_[i]);
        } else if (!is_quant_) {
            // 非量化模型：输出已在 setupZeroCopyIO 中请求转为 float32
            float* src = static_cast<float*>(output_mems_[i]->virt_addr);
            int elem_count = output_attrs_[i].n_elems;
            float_outputs_[i].assign(src, src + elem_count);
        } else {
            LOG_WARN("Output[{}]: unsupported format (quant={}, fmt={}, type={})",
                         i, is_quant_, static_cast<int>(native_output_attrs_[i].fmt),
                         static_cast<int>(native_output_attrs_[i].type));
            return false;
        }
    }

    // 构造 rknn_output 数组指向转换后的 float 数据
    std::vector<rknn_output> outputs(model_info_.num_outputs);
    for (uint32_t i = 0; i < model_info_.num_outputs; ++i) {
        outputs[i].index = i;
        outputs[i].buf = float_outputs_[i].data();
        outputs[i].size = float_outputs_[i].size() * sizeof(float);
    }

    return parseDetectionOutputs(outputs.data(), result);
}

void RKNNDetector::cpuFallbackNV12toRGB(const DMABufferInfo& dma_info) {
    if (!input_mem_ || !input_mem_->virt_addr) {
        LOG_ERROR("cpuFallbackNV12toRGB: input_mem_ not available");
        return;
    }

    // 映射 DMA buffer
    size_t total_size = static_cast<size_t>(dma_info.stride) * dma_info.height * 3 / 2;
    void* mapped = mmap(nullptr, total_size, PROT_READ, MAP_SHARED, dma_info.fd, 0);
    if (mapped == MAP_FAILED) {
        LOG_ERROR("cpuFallbackNV12toRGB: mmap failed, fd={}, size={}", dma_info.fd, total_size);
        return;
    }

    const uint8_t* y_plane = static_cast<const uint8_t*>(mapped);
    const uint8_t* uv_plane = y_plane + dma_info.stride * dma_info.height;

    uint8_t* dst = static_cast<uint8_t*>(input_mem_->virt_addr);
    int src_stride = dma_info.stride > 0 ? dma_info.stride : dma_info.width;

    // 最近邻缩放 + NV12→RGB 转换，直接写入 RKNN 输入内存
    for (int dy = 0; dy < model_info_.input_height; ++dy) {
        int sy = dy * dma_info.height / model_info_.input_height;
        for (int dx = 0; dx < model_info_.input_width; ++dx) {
            int sx = dx * dma_info.width / model_info_.input_width;

            uint8_t y = y_plane[sy * src_stride + sx];
            uint8_t u = uv_plane[(sy / 2) * src_stride + (sx & ~1)];
            uint8_t v = uv_plane[(sy / 2) * src_stride + (sx & ~1) + 1];

            float rf = y + 1.402f * (v - 128.0f);
            float gf = y - 0.344f * (u - 128.0f) - 0.714f * (v - 128.0f);
            float bf = y + 1.772f * (u - 128.0f);

            int idx = (dy * model_info_.input_width + dx) * 3;
            dst[idx] = static_cast<uint8_t>(std::clamp(static_cast<int>(rf), 0, 255));
            dst[idx + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(gf), 0, 255));
            dst[idx + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(bf), 0, 255));
        }
    }

    munmap(mapped, total_size);
}
#endif

} // namespace nvr::detection
