//
// RGA 预处理器 - 使用 RGA 硬件将 NV12 DMA buffer 缩放转换为 RKNN 输入格式
//

#ifndef NVR_RGA_PREPROCESSOR_H
#define NVR_RGA_PREPROCESSOR_H

#include <cstdint>
#include <cstddef>  // RGA headers need NULL

#include <rga/im2d.h>
#include <rga/rga.h>

extern "C" {
#include <rknn_api.h>
}

namespace nvr::detection {

class RGAPreprocessor {
public:
    RGAPreprocessor();
    ~RGAPreprocessor();

    RGAPreprocessor(const RGAPreprocessor&) = delete;
    RGAPreprocessor& operator=(const RGAPreprocessor&) = delete;

    // 初始化（在 RKNN 上下文创建后调用）
    bool initialize(rknn_context rknn_ctx, int model_width, int model_height);

    // 预处理：源 NV12 DMA fd -> RKNN 输入 buffer
    // 返回 RKNN tensor mem（调用者用完后传给 releaseRKNNMem）
    rknn_tensor_mem* preprocess(int src_fd, int src_width, int src_height, int src_format, int src_stride = 0);

    // 释放 RKNN 内存
    void releaseRKNNMem(rknn_tensor_mem* mem);

    bool isInitialized() const { return initialized_; }

private:
    bool initialized_;
    rknn_context rknn_ctx_;
    int model_width_;
    int model_height_;
};

} // namespace nvr::detection

#endif // NVR_RGA_PREPROCESSOR_H
