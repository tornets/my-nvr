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
        logger_->warn("RKNNDetector already initialized");
        return true;
    }

    logger_->info("Initializing RKNNDetector with model: {}", config_.model_path);

    // 加载模型
    if (!loadModel()) {
        logger_->error("Failed to load RKNN model");
        return false;
    }

    // 初始化 RKNN 上下文
    if (!initRKNNContext()) {
        logger_->error("Failed to initialize RKNN context");
        releaseResources();
        return false;
    }

    // 查询模型信息
    if (!queryModelInfo()) {
        logger_->error("Failed to query model info");
        releaseResources();
        return false;
    }

    // 设置输入输出
    if (!setupInputsOutputs()) {
        logger_->error("Failed to setup inputs/outputs");
        releaseResources();
        return false;
    }

    initialized_ = true;
    logger_->info("RKNNDetector initialized successfully");
    logger_->info("Model input: {}x{}x{}", model_info_.input_width, model_info_.input_height, model_info_.input_channels);

    // 初始化 RGA 预处理器（零拷贝模式）
    if (config_.zero_copy_enabled) {
        rga_preprocessor_ = std::make_unique<RGAPreprocessor>();
        if (!rga_preprocessor_->initialize(rknn_ctx_, model_info_.input_width, model_info_.input_height)) {
            logger_->warn("RGA preprocessor init failed, will use CPU fallback");
            rga_preprocessor_.reset();
        }
    }

    return true;
}

void RKNNDetector::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return;
    }

    logger_->info("Shutting down RKNNDetector");
    releaseResources();
    initialized_ = false;
}

bool RKNNDetector::loadModel() {
    std::ifstream file(config_.model_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        logger_->error("Failed to open model file: {}", config_.model_path);
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    model_data_.resize(size);
    if (!file.read(reinterpret_cast<char*>(model_data_.data()), size)) {
        logger_->error("Failed to read model file");
        return false;
    }

    logger_->info("Loaded RKNN model, size: {} bytes", size);
    return true;
}

bool RKNNDetector::initRKNNContext() {
    int ret = rknn_init(&rknn_ctx_, model_data_.data(), model_data_.size(), 0, nullptr);
    if (ret != RKNN_SUCC) {
        logger_->error("rknn_init failed: {}", ret);
        return false;
    }

    logger_->debug("RKNN context initialized");
    return true;
}

bool RKNNDetector::queryModelInfo() {
    rknn_input_output_num io_num_attr;
    int ret = rknn_query(rknn_ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num_attr, sizeof(io_num_attr));
    if (ret < 0) {
        logger_->error("rknn_query RKNN_QUERY_IN_OUT_NUM failed: {}", ret);
        return false;
    }

    logger_->debug("Model input num: {}, output num: {}", io_num_attr.n_input, io_num_attr.n_output);

    // 查询输入属性
    rknn_tensor_attr input_attr;
    input_attr.index = 0;
    ret = rknn_query(rknn_ctx_, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    if (ret < 0) {
        logger_->error("rknn_query RKNN_QUERY_INPUT_ATTR failed: {}", ret);
        return false;
    }

    model_info_.input_width = input_attr.dims[1];
    model_info_.input_height = input_attr.dims[2];
    model_info_.input_channels = input_attr.dims[3];
    model_info_.num_outputs = io_num_attr.n_output;

    logger_->debug("Input: {}x{}x{}", model_info_.input_width, model_info_.input_height, model_info_.input_channels);

    // 查询输出属性
    model_info_.output_sizes.clear();
    for (uint32_t i = 0; i < io_num_attr.n_output; i++) {
        rknn_tensor_attr output_attr;
        output_attr.index = i;
        ret = rknn_query(rknn_ctx_, RKNN_QUERY_OUTPUT_ATTR, &output_attr, sizeof(output_attr));
        if (ret < 0) {
            logger_->error("rknn_query RKNN_QUERY_OUTPUT_ATTR failed for output {}: {}", i, ret);
            return false;
        }
        model_info_.output_sizes.push_back(output_attr.n_elems);
        logger_->debug("Output {}: size = {}", i, output_attr.n_elems);
    }

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

    logger_->debug("Inputs/outputs setup completed ({} outputs)", model_info_.num_outputs);
    return true;
}

void RKNNDetector::releaseResources() {
    // 只销毁 RKNN 上下文（不需要手动 release 未 get 的输出）
    if (rknn_ctx_ != 0) {
        rknn_destroy(rknn_ctx_);
        rknn_ctx_ = 0;
    }

    model_data_.clear();
    outputs_.clear();
    logger_->debug("Resources released");
}

bool RKNNDetector::detectFrame(AVFrame* frame, DetectionResult& result) {
    if (!initialized_) {
        logger_->error("RKNNDetector not initialized");
        return false;
    }

    if (!frame) {
        logger_->error("Invalid frame");
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    // 预处理帧
    std::vector<uint8_t> input_data;
    if (!preprocessFrame(frame, input_data)) {
        logger_->error("Failed to preprocess frame");
        return false;
    }

    // 设置输入
    inputs_[0].buf = input_data.data();
    int ret = rknn_inputs_set(rknn_ctx_, 1, inputs_);
    if (ret < 0) {
        logger_->error("rknn_inputs_set failed: {}", ret);
        return false;
    }

    // 执行推理
    ret = rknn_run(rknn_ctx_, nullptr);
    if (ret < 0) {
        logger_->error("rknn_run failed: {}", ret);
        return false;
    }

    // 获取输出
    ret = rknn_outputs_get(rknn_ctx_, outputs_.size(), outputs_.data(), nullptr);
    if (ret < 0) {
        logger_->error("rknn_outputs_get failed: {}", ret);
        return false;
    }

    // 解析检测结果
    if (!parseDetectionOutputs(outputs_.data(), result)) {
        logger_->error("Failed to parse detection outputs");
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
        logger_->error("RKNNDetector not initialized");
        return false;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    int dst_width = model_info_.input_width;
    int dst_height = model_info_.input_height;

    // mmap NV12 DMA buffer → CPU NV12→RGB 缩放转换
    // TODO: 后续改用 RGA 硬件加速 NV12→RGB 转换
    std::vector<uint8_t> rgb_data(dst_width * dst_height * 3);

    size_t nv12_size = dma_info.size > 0 ? dma_info.size
        : static_cast<size_t>(dma_info.stride * dma_info.height * 3 / 2);

    void* mapped = mmap(nullptr, nv12_size, PROT_READ, MAP_SHARED, dma_info.fd, 0);
    if (mapped == MAP_FAILED) {
        logger_->error("mmap DMA fd {} failed", dma_info.fd);
        return false;
    }

    const uint8_t* y_plane = static_cast<const uint8_t*>(mapped);
    const uint8_t* uv_plane = y_plane + dma_info.stride * dma_info.height;
    int src_stride = dma_info.stride > 0 ? dma_info.stride : dma_info.width;

    for (int dy = 0; dy < dst_height; dy++) {
        int sy = dy * dma_info.height / dst_height;
        for (int dx = 0; dx < dst_width; dx++) {
            int sx = dx * dma_info.width / dst_width;

            uint8_t y  = y_plane[sy * src_stride + sx];
            uint8_t u  = uv_plane[(sy / 2) * src_stride + (sx & ~1)];
            uint8_t v  = uv_plane[(sy / 2) * src_stride + (sx & ~1) + 1];

            // BT.601 full range YUV→RGB
            float rf = y + 1.402f * (v - 128.0f);
            float gf = y - 0.344f * (u - 128.0f) - 0.714f * (v - 128.0f);
            float bf = y + 1.772f * (u - 128.0f);

            int idx = (dy * dst_width + dx) * 3;
            rgb_data[idx]     = static_cast<uint8_t>(std::clamp(static_cast<int>(rf), 0, 255));
            rgb_data[idx + 1] = static_cast<uint8_t>(std::clamp(static_cast<int>(gf), 0, 255));
            rgb_data[idx + 2] = static_cast<uint8_t>(std::clamp(static_cast<int>(bf), 0, 255));
        }
    }

    munmap(mapped, nv12_size);

    inputs_[0].buf = rgb_data.data();
    inputs_[0].size = dst_width * dst_height * 3;
    int ret = rknn_inputs_set(rknn_ctx_, 1, inputs_);
    if (ret < 0) {
        logger_->error("rknn_inputs_set failed: {}", ret);
        return false;
    }

    ret = rknn_run(rknn_ctx_, nullptr);
    if (ret < 0) {
        logger_->error("rknn_run failed: {}", ret);
        return false;
    }

    ret = rknn_outputs_get(rknn_ctx_, outputs_.size(), outputs_.data(), nullptr);
    if (ret < 0) {
        logger_->error("rknn_outputs_get failed: {}", ret);
        return false;
    }

    bool parse_ok = parseDetectionOutputs(outputs_.data(), result);
    rknn_outputs_release(rknn_ctx_, outputs_.size(), outputs_.data());

    if (!parse_ok) {
        logger_->error("Failed to parse detection outputs");
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
        logger_->error("Failed to create SwsContext");
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
        logger_->error("Invalid outputs");
        return false;
    }

    // YOLO11 RKNN 输出格式（9 个输出，3 个检测头 × 3 输出）:
    //   [3*i]:   DFL bbox  [dfl_len*4, grid_h, grid_w]
    //   [3*i+1]: class scores [num_classes, grid_h, grid_w]
    //   [3*i+2]: score sum [1, grid_h, grid_w] (快速过滤)
    //
    // 数据布局: channel-first (NCHW), 如 score_tensor[c * grid_len + offset]
    // 参考官方示例: rknn_model_zoo/examples/yolo11/cpp/postprocess.cc

    const int num_classes = 80;
    const int reg_max = model_info_.output_sizes[0] / (model_info_.output_sizes[2]) / 4;
    // reg_max = 409600 / 6400 / 4 = 16

    std::vector<BoundingBox> all_boxes;

    for (int s = 0; s < 3; s++) {
        if (3 * s + 2 >= static_cast<int>(model_info_.output_sizes.size())) break;

        float* bbox_dfl = static_cast<float*>(outputs[3 * s].buf);
        float* cls_scores = static_cast<float*>(outputs[3 * s + 1].buf);
        float* score_sum = static_cast<float*>(outputs[3 * s + 2].buf);

        int grid_len = model_info_.output_sizes[3 * s + 2];  // grid_h * grid_w
        int grid = 1;
        while (grid * grid < grid_len) grid++;
        int grid_h = grid;
        int grid_w = grid;
        int stride = model_info_.input_height / grid_h;

        for (int i = 0; i < grid_h; i++) {
            for (int j = 0; j < grid_w; j++) {
                int offset = i * grid_w + j;

                // 快速过滤: score sum < threshold 则跳过
                if (score_sum[offset] < config_.confidence_threshold) {
                    continue;
                }

                // 找最大 class score（channel-first: score_tensor[c * grid_len + offset]）
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

                // DFL 解码 bbox（channel-first: box_tensor[k * grid_len + offset]）
                float box[4];
                float before_dfl[reg_max * 4];
                int dfl_offset = offset;
                for (int k = 0; k < reg_max * 4; k++) {
                    before_dfl[k] = bbox_dfl[dfl_offset];
                    dfl_offset += grid_len;
                }

                // compute_dfl: 4 组 reg_max softmax
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
        logger_->error("Cannot warmup: detector not initialized");
        return false;
    }

    logger_->info("Warming up RKNN detector with {} iterations...", iterations);

    // 创建虚拟输入
    std::vector<uint8_t> dummy_input(
        model_info_.input_width * model_info_.input_height * model_info_.input_channels,
        128);

    for (int i = 0; i < iterations; i++) {
        inputs_[0].buf = dummy_input.data();

        int ret = rknn_inputs_set(rknn_ctx_, 1, inputs_);
        if (ret < 0) {
            logger_->error("Warmup failed at iteration {}", i);
            return false;
        }

        ret = rknn_run(rknn_ctx_, nullptr);
        if (ret < 0) {
            logger_->error("Warmup failed at iteration {}", i);
            return false;
        }

        ret = rknn_outputs_get(rknn_ctx_, outputs_.size(), outputs_.data(), nullptr);
        if (ret < 0) {
            logger_->error("Warmup failed at iteration {}", i);
            return false;
        }

        rknn_outputs_release(rknn_ctx_, outputs_.size(), outputs_.data());
    }

    logger_->info("Warmup completed successfully");
    return true;
}

} // namespace nvr::detection
