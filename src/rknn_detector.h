//
// Created by Claude on 2026/4/19.
// RKNN 检测器 - 封装 RKNN 模型加载、推理和结果解析
//

#ifndef NVR_RKNN_DETECTOR_H
#define NVR_RKNN_DETECTOR_H

#include "detection_types.h"
#include "rga_preprocessor.h"
#include <string>
#include <memory>
#include <mutex>
#include "log.h"

extern "C" {
#include <rknn_api.h>
}

// 前向声明
struct AVFrame;
struct AVDRMFrameDescriptor;

namespace nvr::detection {

// RKNN 模型信息
struct RKNNModelInfo {
    int input_width;
    int input_height;
    int input_channels;
    int num_outputs;
    std::vector<int> output_sizes;

    RKNNModelInfo() : input_width(0), input_height(0), input_channels(0), num_outputs(0) {}
};

// RKNN 检测器
class RKNNDetector {
public:
    explicit RKNNDetector(const DetectionConfig& config);
    ~RKNNDetector();

    // 禁止拷贝
    RKNNDetector(const RKNNDetector&) = delete;
    RKNNDetector& operator=(const RKNNDetector&) = delete;

    // 初始化检测器
    bool initialize();
    void shutdown();

    // 检查是否已初始化
    bool isInitialized() const { return initialized_; }

    // 使用 CPU 拷贝方式进行检测
    bool detectFrame(AVFrame* frame, DetectionResult& result, int64_t frame_pts = 0);

    // 使用零拷贝方式进行检测
    bool detectFrameZeroCopy(const DMABufferInfo& dma_info, DetectionResult& result, int64_t frame_pts = 0);

    // 获取模型信息
    const RKNNModelInfo& getModelInfo() const { return model_info_; }

    // 获取配置
    const DetectionConfig& getConfig() const { return config_; }

    // 获取统计信息
    const DetectionStats& getStats() const { return stats_; }
    void resetStats() { stats_.reset(); }

    // 设置 NPU 核心亲和性，须在 initialize() 之后调用
    bool setCoreMask(uint32_t core_mask);  // 传入 RKNN_NPU_CORE_0/1/2 或组合

    // 调试：获取最后一次推理的输入 RGB 数据（640×640）
    const std::vector<uint8_t>& getLastInputRGB() const { return last_input_rgb_; }
    int getLastInputWidth() const { return last_input_w_; }
    int getLastInputHeight() const { return last_input_h_; }

    // 预热模型（运行几次推理以优化性能）
    bool warmup(int iterations = 3);

private:
    // 加载模型
    bool loadModel();

    // 初始化 RKNN 上下文
    bool initRKNNContext();

    // 获取模型信息
    bool queryModelInfo();

    // 设置输入输出
    bool setupInputsOutputs();

    // 从 AVFrame 提取并预处理图像数据
    bool preprocessFrame(AVFrame* frame, std::vector<uint8_t>& output_data);

    // 解析检测输出
    bool parseDetectionOutputs(rknn_output* outputs, DetectionResult& result);

    // YOLO11 后处理
    void yolo11PostProcess(
        float* output_data,
        int output_size,
        DetectionResult& result);

    // 应用 NMS (Non-Maximum Suppression)
    std::vector<BoundingBox> applyNMS(
        const std::vector<BoundingBox>& boxes,
        float iou_threshold = 0.45f);

    // 释放资源
    void releaseResources();

    // 日志
    std::shared_ptr<spdlog::logger> logger_;

    // 配置
    DetectionConfig config_;

    // RKNN 上下文
    rknn_context rknn_ctx_;
    bool initialized_;

    // 模型数据
    std::vector<uint8_t> model_data_;

    // 模型信息
    RKNNModelInfo model_info_;

    // 输入输出
    rknn_input inputs_[1];
    std::vector<rknn_output> outputs_;

    // 统计信息
    DetectionStats stats_;

    // RGA 预处理器（零拷贝）
    std::unique_ptr<RGAPreprocessor> rga_preprocessor_;

    // 线程安全
    mutable std::mutex mutex_;

    // 调试：存储最后一次推理的输入 RGB
    std::vector<uint8_t> last_input_rgb_;
    int last_input_w_ = 0;
    int last_input_h_ = 0;

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 零拷贝：持久化 RKNN 输入/输出内存
    rknn_tensor_mem* input_mem_ = nullptr;
    std::vector<rknn_tensor_mem*> output_mems_;

    // NATIVE 属性（NC1HWC2 格式，含 w_stride/zp/scale）
    std::vector<rknn_tensor_attr> native_input_attrs_;
    std::vector<rknn_tensor_attr> native_output_attrs_;

    // 用户面输出属性（NCHW 逻辑尺寸，用于后处理）
    std::vector<rknn_tensor_attr> output_attrs_;

    // NC1HWC2→NCHW 转换缓冲区（一次分配，每帧复用）
    std::vector<std::vector<float>> float_outputs_;
    bool is_quant_ = false;

    // 零拷贝方法
    bool setupZeroCopyIO();
    void releaseZeroCopyMem();
    void convertNC1HWC2ToFloat(int output_idx, std::vector<float>& out_buf);
    bool parseDetectionOutputsZeroCopy(DetectionResult& result);
    void cpuFallbackNV12toRGB(const DMABufferInfo& dma_info);

    // Letterbox 参数（CPU 模式）
    LetterboxParams last_letterbox_params_;

    // 映射检测坐标到原始帧空间
    void mapCoordinatesToOriginalFrame(
        DetectionResult& result,
        const LetterboxParams& letterbox,
        int original_width,
        int original_height);
#endif
};

} // namespace nvr::detection

#endif // NVR_RKNN_DETECTOR_H
