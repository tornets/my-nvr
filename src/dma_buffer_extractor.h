//
// Created by Claude on 2026/4/19.
// DMA Buffer 提取器 - 用于零拷贝的 DMA buffer 管理
//

#ifndef NVR_DMA_BUFFER_EXTRACTOR_H
#define NVR_DMA_BUFFER_EXTRACTOR_H

#include "detection_types.h"
#include <rknn_api.h>
#include <memory>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
}

namespace nvr::detection {

// DMA Buffer 包装器（自动管理生命周期）
class DMABufferWrapper {
public:
    DMABufferWrapper();
    explicit DMABufferWrapper(int fd, size_t size, int width, int height, int format, int stride = 0);
    ~DMABufferWrapper();

    // 禁止拷贝
    DMABufferWrapper(const DMABufferWrapper&) = delete;
    DMABufferWrapper& operator=(const DMABufferWrapper&) = delete;

    // 移动构造
    DMABufferWrapper(DMABufferWrapper&& other) noexcept;

    // 移动赋值
    DMABufferWrapper& operator=(DMABufferWrapper&& other) noexcept;

    // 获取信息
    int getFD() const { return info_.fd; }
    size_t getSize() const { return info_.size; }
    int getWidth() const { return info_.width; }
    int getHeight() const { return info_.height; }
    int getFormat() const { return info_.format; }
    const DMABufferInfo& getInfo() const { return info_; }

    // 检查是否有效
    bool isValid() const { return info_.fd >= 0; }

    // 释放资源
    void release();

private:
    DMABufferInfo info_;
};

// DMA Buffer 提取器
class DMABufferExtractor {
public:
    DMABufferExtractor() = default;
    ~DMABufferExtractor() = default;

    // 禁止拷贝
    DMABufferExtractor(const DMABufferExtractor&) = delete;
    DMABufferExtractor& operator=(const DMABufferExtractor&) = delete;

    // 从 AVFrame 提取 DMA buffer
    static std::unique_ptr<DMABufferWrapper> extractFromAVFrame(AVFrame* frame);

    // 检查 AVFrame 是否包含 DMA buffer
    static bool hasDMABuffer(AVFrame* frame);

    // 获取 DRM PRIME 描述符
    static AVDRMFrameDescriptor* getDRMDescriptor(AVFrame* frame);

    // 打印 DMA buffer 信息（调试用）
    static void printDMABufferInfo(AVFrame* frame);

    // 验证 DMA buffer 有效性
    static bool validateDMABuffer(const DMABufferInfo& info);

private:
    // 从 DRM 层提取 DMA buffer 信息
    static bool extractFromDRMLayer(
        const AVDRMFrameDescriptor* drm_desc,
        const AVDRMLayerDescriptor* layer,
        DMABufferInfo& info,
        int width,
        int height);
};

// DMA Buffer 导出器（用于 RKNN 导入）
class DMABufferExporter {
public:
    // 为 RKNN 准备内存描述符
    static bool prepareForRKNN(const DMABufferInfo& info, rknn_tensor_mem& mem_desc);

    // 从 AVFrame 直接准备 RKNN 内存描述符
    static std::unique_ptr<DMABufferWrapper> prepareAVFrameForRKNN(
        AVFrame* frame,
        rknn_tensor_mem& mem_desc);
};

} // namespace nvr::detection

#endif // NVR_DMA_BUFFER_EXTRACTOR_H
