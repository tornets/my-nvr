//
// RGA 预处理器实现
//

#include "rga_preprocessor.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace nvr::detection {

RGAPreprocessor::RGAPreprocessor()
    : initialized_(false)
    , rknn_ctx_(0)
    , model_width_(0)
    , model_height_(0) {}

RGAPreprocessor::~RGAPreprocessor() {
    initialized_ = false;
}

bool RGAPreprocessor::initialize(rknn_context rknn_ctx, int model_width, int model_height) {
    if (rknn_ctx == 0) {
        spdlog::error("RGAPreprocessor: invalid rknn context");
        return false;
    }

    rknn_ctx_ = rknn_ctx;
    model_width_ = model_width;
    model_height_ = model_height;
    initialized_ = true;

    spdlog::info("RGAPreprocessor initialized: target {}x{}", model_width_, model_height_);
    return true;
}

rknn_tensor_mem* RGAPreprocessor::preprocess(int src_fd, int src_width, int src_height, int src_format, int src_stride) {
    if (!initialized_ || src_fd < 0) {
        return nullptr;
    }

    // 如果没有提供 stride，默认使用 width
    int actual_stride = (src_stride > 0) ? src_stride : src_width;

    // 对于 NV12，importbuffer_fd 需要完整的 buffer 宽高
    // RGA 内部知道 NV12 格式：Y 平面 stride*height，UV 平面 stride*(height/2)
    int import_height = src_height;
    int import_width = actual_stride;

    spdlog::debug("RGA preprocess: src_fd={}, {}x{}, stride={}, format=0x{:x}",
                  src_fd, src_width, src_height, actual_stride, src_format);

    // 1. 分配 RKNN 目标内存（640x640x3 = RGB_888）
    int dst_size = model_width_ * model_height_ * 3;
    rknn_tensor_mem* rknn_mem = rknn_create_mem(rknn_ctx_, dst_size);
    if (!rknn_mem) {
        spdlog::error("RGAPreprocessor: rknn_create_mem failed");
        return nullptr;
    }

    // 2. 导入源 buffer 到 RGA
    rga_buffer_handle_t src_handle = importbuffer_fd(src_fd, import_width, import_height, src_format);
    if (!src_handle) {
        spdlog::error("RGAPreprocessor: importbuffer_fd(src) failed, fd={}, {}x{}, format=0x{:x}",
                       src_fd, import_width, import_height, src_format);
        rknn_destroy_mem(rknn_ctx_, rknn_mem);
        return nullptr;
    }

    // 3. 导入 RKNN 目标 buffer 到 RGA
    rga_buffer_handle_t dst_handle = importbuffer_fd(rknn_mem->fd, model_width_, model_height_, RK_FORMAT_RGB_888);
    if (!dst_handle) {
        spdlog::error("RGAPreprocessor: importbuffer_fd(dst) failed, fd={}, {}x{}",
                       rknn_mem->fd, model_width_, model_height_);
        releasebuffer_handle(src_handle);
        rknn_destroy_mem(rknn_ctx_, rknn_mem);
        return nullptr;
    }

    // 4. 构造 RGA buffer 描述
    //    对于 NV12：image_width 是实际裁剪宽度，wstride 是 buffer 行宽
    rga_buffer_t src_buf = wrapbuffer_handle_t(src_handle, src_width, src_height,
                                                import_width, import_height, src_format);
    rga_buffer_t dst_buf = wrapbuffer_handle_t(dst_handle, model_width_, model_height_,
                                                model_width_, model_height_, RK_FORMAT_RGB_888);

    // 5. 使用 improcess 进行缩放+色彩转换（NV12→RGB）
    im_rect empty_rect = {};
    IM_STATUS status = improcess(src_buf, dst_buf, {}, empty_rect, empty_rect, empty_rect,
                                 IM_YUV_TO_RGB_BT601_LIMIT | IM_SYNC);

    // 6. 释放 RGA 句柄
    releasebuffer_handle(src_handle);
    releasebuffer_handle(dst_handle);

    if (status != IM_STATUS_SUCCESS) {
        spdlog::error("RGAPreprocessor: improcess failed with status {} (src={}x{}, dst={}x{}, stride={})",
                       static_cast<int>(status), src_width, src_height, model_width_, model_height_, actual_stride);
        rknn_destroy_mem(rknn_ctx_, rknn_mem);
        return nullptr;
    }

    // 验证输出：检查 RKNN buffer 前 16 字节是否看起来像有效 RGB
    if (rknn_mem->virt_addr) {
        uint8_t* rgb = static_cast<uint8_t*>(rknn_mem->virt_addr);
        int non_zero = 0;
        for (int i = 0; i < 16; i++) {
            if (rgb[i] != 0) non_zero++;
        }
        spdlog::debug("RGA output first 16 bytes: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} "
                       "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} (non_zero={})",
                       rgb[0], rgb[1], rgb[2], rgb[3], rgb[4], rgb[5], rgb[6], rgb[7],
                       rgb[8], rgb[9], rgb[10], rgb[11], rgb[12], rgb[13], rgb[14], rgb[15], non_zero);
    }

    return rknn_mem;
}

void RGAPreprocessor::releaseRKNNMem(rknn_tensor_mem* mem) {
    if (mem && rknn_ctx_) {
        rknn_destroy_mem(rknn_ctx_, mem);
    }
}

} // namespace nvr::detection
