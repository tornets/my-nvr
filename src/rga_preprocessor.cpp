//
// RGA 预处理器实现
//

#include "rga_preprocessor.h"
#include "log.h"

namespace nvr::detection {

// DRM 格式（FOURCC）定义
#define DRM_FORMAT_NV12       0x3231564e  // 'NV12'

// DRM 格式到 RGA 格式的映射
int RGAPreprocessor::drmToRGAFormat(int drm_format) {
    switch (drm_format) {
        case DRM_FORMAT_NV12:
            return RK_FORMAT_YCbCr_420_SP;
        default:
            LOG_WARN("Unknown DRM format: 0x{:x}, defaulting to NV12", drm_format);
            return RK_FORMAT_YCbCr_420_SP;
    }
}

RGAPreprocessor::RGAPreprocessor() = default;

RGAPreprocessor::~RGAPreprocessor() {
    if (dst_handle_) {
        releasebuffer_handle(dst_handle_);
        dst_handle_ = 0;
    }
}

bool RGAPreprocessor::initialize(int dst_fd, int model_width, int model_height, int dst_wstride) {
    if (dst_fd <= 0) {
        LOG_ERROR("RGAPreprocessor: invalid dst_fd");
        return false;
    }

    dst_fd_ = dst_fd;
    model_width_ = model_width;
    model_height_ = model_height;
    dst_wstride_ = dst_wstride;

    // 导入 RKNN 输入内存 fd 为 RGA 目标 handle（持久化，每帧复用）
    im_handle_param_t dst_param;
    dst_param.width = model_width_;
    dst_param.height = model_height_;
    dst_param.format = RK_FORMAT_RGB_888;

    dst_handle_ = importbuffer_fd(dst_fd_, &dst_param);
    if (dst_handle_ == 0) {
        LOG_ERROR("RGAPreprocessor: importbuffer_fd(dst) failed, fd={}, {}x{}",
                       dst_fd_, model_width_, model_height_);
        return false;
    }

    // 创建持久化目标 buffer 描述
    dst_buf_ = wrapbuffer_handle_t(dst_handle_, model_width_, model_height_,
                                    dst_wstride_, model_height_, RK_FORMAT_RGB_888);

    initialized_ = true;
    LOG_INFO("RGAPreprocessor initialized: dst_fd={}, {}x{}, dst_wstride={}",
                 dst_fd_, model_width_, model_height_, dst_wstride_);
    return true;
}

bool RGAPreprocessor::resizeNV12toRGB(int src_fd, int src_width, int src_height,
                                       int src_format, int src_wstride, int src_hstride) {
    if (!initialized_ || src_fd < 0) {
        return false;
    }

    // 将 DRM 格式（FOURCC）转换为 RGA 格式
    int rga_format = drmToRGAFormat(src_format);

    // 导入源 DMA fd 为 RGA handle（每帧调用，因为 fd 会变化）
    im_handle_param_t src_param;
    src_param.width = src_width;
    src_param.height = src_height;
    src_param.format = rga_format;

    rga_buffer_handle_t src_handle = importbuffer_fd(src_fd, &src_param);
    if (src_handle == 0) {
        LOG_ERROR("RGAPreprocessor: importbuffer_fd(src) failed, fd={}, img={}x{}, format=0x{:x}",
                       src_fd, src_width, src_height, rga_format);
        return false;
    }

    // 包装源 buffer，传递 stride 信息
    rga_buffer_t src_buf = wrapbuffer_handle_t(src_handle, src_width, src_height,
                                                src_wstride, src_hstride, rga_format);

    // RGA 缩放+色彩转换（NV12→RGB）
    IM_STATUS status = imresize(src_buf, dst_buf_);

    // 立即释放源 handle（dst_handle_ 持久化）
    releasebuffer_handle(src_handle);

    if (status != IM_STATUS_SUCCESS) {
        LOG_ERROR("RGAPreprocessor: imresize failed, status={}, src={}x{}, dst={}x{}",
                       static_cast<int>(status), src_width, src_height,
                       model_width_, model_height_);
        return false;
    }

    return true;
}

} // namespace nvr::detection
