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
#include <spdlog/spdlog.h>

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
    bool detectFrame(AVFrame* frame, DetectionResult& result);

    // 使用零拷贝方式进行检测
    bool detectFrameZeroCopy(const DMABufferInfo& dma_info, DetectionResult& result);

    // 获取模型信息
    const RKNNModelInfo& getModelInfo() const { return model_info_; }

    // 获取配置
    const DetectionConfig& getConfig() const { return config_; }

    // 获取统计信息
    const DetectionStats& getStats() const { return stats_; }
    void resetStats() { stats_.reset(); }

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
};

} // namespace nvr::detection

#endif // NVR_RKNN_DETECTOR_H
