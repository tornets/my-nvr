//
// RGA 预处理器 - 使用 RGA 硬件将 NV12 DMA buffer 缩放转换为 RKNN 输入格式
// 零拷贝模式：RGA 直接写入持久化的 RKNN 输入内存
//

#ifndef NVR_RGA_PREPROCESSOR_H
#define NVR_RGA_PREPROCESSOR_H

#include <cstdint>
#include <cstddef>

#include <rga/im2d.h>
#include <rga/rga.h>
#include "detection_types.h"

namespace nvr::detection {

class RGAPreprocessor {
public:
    RGAPreprocessor();
    ~RGAPreprocessor();

    RGAPreprocessor(const RGAPreprocessor&) = delete;
    RGAPreprocessor& operator=(const RGAPreprocessor&) = delete;

    // 初始化：dst_fd 是 RKNN 输入内存的 fd，dst_virt_addr 是虚拟地址（用于 CPU 回退），dst_wstride 来自 native_input_attrs
    bool initialize(int dst_fd, void* dst_virt_addr, int model_width, int model_height, int dst_wstride);

    // 每帧预处理：NV12 DMA fd → RGB 写入持久化的 RKNN 输入内存
    bool resizeNV12toRGB(int src_fd, int src_width, int src_height,
                         int src_format, int src_wstride, int src_hstride);

    bool isInitialized() const { return initialized_; }

    // 获取 letterbox 参数
    const LetterboxParams& getLetterboxParams() const { return last_letterbox_params_; }

private:
    // 将 DRM 格式（FOURCC）转换为 RGA 格式
    static int drmToRGAFormat(int drm_format);

    bool initialized_ = false;
    int dst_fd_ = 0;
    void* dst_virt_addr_ = nullptr;       // RKNN 输入内存虚拟地址（用于 CPU 回退填充）
    int model_width_ = 0;
    int model_height_ = 0;
    int dst_wstride_ = 0;
    LetterboxParams last_letterbox_params_;  // 最后一次 letterbox 参数
};

} // namespace nvr::detection

#endif // NVR_RGA_PREPROCESSOR_H
