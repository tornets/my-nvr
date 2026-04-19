//
// Created by Claude on 2026/4/19.
// DMA Buffer 提取器实现
//

#include "dma_buffer_extractor.h"
#include <spdlog/spdlog.h>
#include <cstring>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace nvr::detection {

// ============================================================================
// DMABufferWrapper 实现
// ============================================================================

DMABufferWrapper::DMABufferWrapper()
    : info_()
{
    info_.fd = -1;
    info_.size = 0;
    info_.width = 0;
    info_.height = 0;
    info_.format = 0;
}

DMABufferWrapper::DMABufferWrapper(int fd, size_t size, int width, int height, int format, int stride)
    : info_()
{
    info_.fd = fd;
    info_.size = size;
    info_.width = width;
    info_.height = height;
    info_.format = format;
    info_.stride = stride;

    spdlog::trace("DMABufferWrapper created: fd={}, size={}x{}, format={}, stride={}",
                  fd, width, height, format, stride);
}

DMABufferWrapper::~DMABufferWrapper() {
    release();
}

DMABufferWrapper::DMABufferWrapper(DMABufferWrapper&& other) noexcept
    : info_(other.info_)
{
    // 清空源对象
    other.info_.fd = -1;
    other.info_.size = 0;
    other.info_.width = 0;
    other.info_.height = 0;
    other.info_.format = 0;
    other.info_.stride = 0;
}

DMABufferWrapper& DMABufferWrapper::operator=(DMABufferWrapper&& other) noexcept {
    if (this != &other) {
        // 释放现有资源
        release();

        // 清空源对象
        other.info_.fd = -1;
        other.info_.size = 0;
        other.info_.width = 0;
        other.info_.height = 0;
        other.info_.format = 0;
        other.info_.stride = 0;
    }
    return *this;
}

void DMABufferWrapper::release() {
    if (info_.fd >= 0) {
        spdlog::trace("Releasing DMA buffer: fd={}", info_.fd);
        // 注意：DMA buffer 由 FFmpeg 管理，这里不应关闭 fd
        // 如果需要导出 fd，可以使用 close(info_.fd)
        info_.fd = -1;
    }
}

// ============================================================================
// DMABufferExtractor 实现
// ============================================================================

std::unique_ptr<DMABufferWrapper> DMABufferExtractor::extractFromAVFrame(AVFrame* frame) {
    if (!frame) {
        spdlog::error("Cannot extract DMA buffer: frame is null");
        return nullptr;
    }

    if (!hasDMABuffer(frame)) {
        spdlog::warn("Frame does not contain DMA buffer");
        return nullptr;
    }

    AVDRMFrameDescriptor* drm_desc = getDRMDescriptor(frame);
    if (!drm_desc) {
        spdlog::error("Failed to get DRM descriptor");
        return nullptr;
    }

    // 遍历层和对象
    for (int layer = 0; layer < drm_desc->nb_layers; ++layer) {
        const AVDRMLayerDescriptor& layer_desc = drm_desc->layers[layer];

        for (int plane = 0; plane < layer_desc.nb_planes; ++plane) {
            const AVDRMObjectDescriptor& object = drm_desc->objects[layer_desc.planes[plane].object_index];

            spdlog::debug("Extracted DMA buffer: layer={}, plane={}, fd={}, size={} bytes",
                         layer, plane, object.fd, object.size);
        }

        // 只取第一层第一个平面的 fd 和 stride（NV12 两个平面共享同一 fd）
        const AVDRMObjectDescriptor& object = drm_desc->objects[layer_desc.planes[0].object_index];
        int stride = static_cast<int>(layer_desc.planes[0].pitch);

        auto wrapper = std::make_unique<DMABufferWrapper>(
            object.fd,
            object.size,
            frame->width,
            frame->height,
            frame->format,
            stride
        );

        if (wrapper && wrapper->isValid()) {
            return wrapper;
        }
    }

    spdlog::error("Failed to extract valid DMA buffer from frame");
    return nullptr;
}

bool DMABufferExtractor::hasDMABuffer(AVFrame* frame) {
    if (!frame) {
        return false;
    }

    // 检查帧格式是否为 DRM_PRIME
    return frame->format == AV_PIX_FMT_DRM_PRIME;
}

AVDRMFrameDescriptor* DMABufferExtractor::getDRMDescriptor(AVFrame* frame) {
    if (!frame || !hasDMABuffer(frame)) {
        return nullptr;
    }

    return reinterpret_cast<AVDRMFrameDescriptor*>(frame->data[0]);
}

void DMABufferExtractor::printDMABufferInfo(AVFrame* frame) {
    if (!hasDMABuffer(frame)) {
        spdlog::info("Frame does not contain DMA buffer");
        return;
    }

    AVDRMFrameDescriptor* drm_desc = getDRMDescriptor(frame);
    if (!drm_desc) {
        return;
    }

    spdlog::info("DMA Buffer Info:");
    spdlog::info("  Format: {}", av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
    spdlog::info("  Width: {}", frame->width);
    spdlog::info("  Height: {}", frame->height);
    spdlog::info("  Layers: {}", drm_desc->nb_layers);
    spdlog::info("  Objects: {}", drm_desc->nb_objects);

    for (int i = 0; i < drm_desc->nb_objects; ++i) {
        const auto& obj = drm_desc->objects[i];
        spdlog::info("    Object[{}]: fd={}, size={}, format_modifier=0x{:x}",
                     i, obj.fd, obj.size, obj.format_modifier);
    }

    for (int i = 0; i < drm_desc->nb_layers; ++i) {
        const auto& layer = drm_desc->layers[i];
        spdlog::info("    Layer[{}]: nb_planes={}, format={}",
                     i, layer.nb_planes, layer.format);

        for (int j = 0; j < layer.nb_planes; ++j) {
            spdlog::info("      Plane[{}]: offset={}, pitch={}",
                         j, layer.planes[j].offset, layer.planes[j].pitch);
        }
    }
}

bool DMABufferExtractor::validateDMABuffer(const DMABufferInfo& info) {
    if (info.fd < 0) {
        spdlog::error("Invalid DMA buffer: fd < 0");
        return false;
    }

    if (info.size == 0) {
        spdlog::error("Invalid DMA buffer: size == 0");
        return false;
    }

    if (info.width <= 0 || info.height <= 0) {
        spdlog::error("Invalid DMA buffer: invalid dimensions {}x{}", info.width, info.height);
        return false;
    }

    return true;
}

bool DMABufferExtractor::extractFromDRMLayer(
    const AVDRMFrameDescriptor* drm_desc,
    const AVDRMLayerDescriptor* layer,
    DMABufferInfo& info,
    int width,
    int height) {

    if (!drm_desc || !layer || layer->nb_planes == 0) {
        return false;
    }

    // 获取第一个平面对应的对象
    const AVDRMObjectDescriptor& object = drm_desc->objects[layer->planes[0].object_index];

    info.fd = object.fd;
    info.size = object.size;
    info.width = width;
    info.height = height;
    info.format = layer->format;

    return true;
}

// ============================================================================
// DMABufferExporter 实现
// ============================================================================

bool DMABufferExporter::prepareForRKNN(const DMABufferInfo& info, rknn_tensor_mem& mem_desc) {
    if (!DMABufferExtractor::validateDMABuffer(info)) {
        return false;
    }

    std::memset(&mem_desc, 0, sizeof(mem_desc));

    mem_desc.fd = info.fd;
    mem_desc.size = info.size;
    mem_desc.virt_addr = nullptr;  // 零拷贝模式，虚拟地址为空

    spdlog::debug("Prepared DMA buffer for RKNN: fd={}, size={}", info.fd, info.size);

    return true;
}

std::unique_ptr<DMABufferWrapper> DMABufferExporter::prepareAVFrameForRKNN(
    AVFrame* frame,
    rknn_tensor_mem& mem_desc) {

    // 提取 DMA buffer
    auto wrapper = DMABufferExtractor::extractFromAVFrame(frame);
    if (!wrapper) {
        return nullptr;
    }

    // 准备 RKNN 内存描述符
    if (!prepareForRKNN(wrapper->getInfo(), mem_desc)) {
        return nullptr;
    }

    return wrapper;
}

} // namespace nvr::detection
