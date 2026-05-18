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

RGAPreprocessor::~RGAPreprocessor() = default;

bool RGAPreprocessor::initialize(int dst_fd, void* dst_virt_addr, int model_width, int model_height, int dst_wstride) {
    if (dst_fd <= 0) {
        LOG_ERROR("RGAPreprocessor: invalid dst_fd");
        return false;
    }

    dst_fd_ = dst_fd;
    dst_virt_addr_ = dst_virt_addr;
    model_width_ = model_width;
    model_height_ = model_height;
    dst_wstride_ = dst_wstride;

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
    int src_fmt = drmToRGAFormat(src_format);
    int dst_fmt = RK_FORMAT_RGB_888;

    // 1. 计算 letterbox 参数（完全参考官方 image_utils.c:699-792）
    float scale_w = static_cast<float>(model_width_) / src_width;
    float scale_h = static_cast<float>(model_height_) / src_height;
    float scale = std::min(scale_w, scale_h);

    int resize_w = static_cast<int>(src_width * scale);
    int resize_h = static_cast<int>(src_height * scale);

    // 对齐调整（allow_slight_change）
    const int allow_slight_change = 1;
    if (allow_slight_change == 1 && (resize_w % 4 != 0)) {
        resize_w -= resize_w % 4;
    }
    if (allow_slight_change == 1 && (resize_h % 2 != 0)) {
        resize_h -= resize_h % 2;
    }

    int padding_w = model_width_ - resize_w;
    int padding_h = model_height_ - resize_h;

    // 计算填充偏移（完全参考官方）
    int pad_x = 0, pad_y = 0;
    im_rect dst_box;
    dst_box.x = 0;
    dst_box.y = 0;
    dst_box.width = model_width_;
    dst_box.height = model_height_;

    if (scale_w < scale_h) {
        // 宽度限制，上下填充
        pad_y = padding_h / 2;
        if (pad_y % 2 != 0) {
            pad_y -= pad_y % 2;
            if (pad_y < 0) {
                pad_y = 0;
            }
        }
        dst_box.y = pad_y;
        dst_box.height = resize_h;
        dst_box.width = model_width_;  // 宽度占满
    } else {
        // 高度限制，左右填充
        pad_x = padding_w / 2;
        if (pad_x % 2 != 0) {
            pad_x -= pad_x % 2;
            if (pad_x < 0) {
                pad_x = 0;
            }
        }
        dst_box.x = pad_x;
        dst_box.width = resize_w;
        dst_box.height = model_height_;  // 高度占满
    }

    // 存储 letterbox 参数
    last_letterbox_params_.scale = scale;
    last_letterbox_params_.pad_x = pad_x;
    last_letterbox_params_.pad_y = pad_y;

    LOG_DEBUG("RGA Letterbox: src={}x{} (stride={}x{}), dst={}x{} (wstride={}), scale={}, pad_x={}, pad_y={}, resize={}x{}",
              src_width, src_height, src_wstride, src_hstride,
              model_width_, model_height_, dst_wstride_,
              scale, pad_x, pad_y, resize_w, resize_h);

    // 2. 包装 buffer（注意：NV12 的 stride 参数很关键）
    // 对于源 NV12 buffer，使用实际的 stride
    rga_buffer_t rga_buf_src = wrapbuffer_fd(src_fd, src_width, src_height, src_fmt, src_wstride, src_hstride);
    // 对于目标 RGB buffer，使用 model_width_ 作为 width stride
    rga_buffer_t rga_buf_dst = wrapbuffer_fd(dst_fd_, model_width_, model_height_, dst_fmt, dst_wstride_, model_height_);

    // 3. 先用 CPU memset 填充黑色背景（更可靠）
    // 注意：需要使用 dst_wstride_ 计算实际大小
    LOG_DEBUG("Filling background: virt_addr={}, dst_wstride_={}, size={}",
              fmt::ptr(dst_virt_addr_), dst_wstride_, dst_wstride_ * model_height_ * 3);
    if (dst_virt_addr_ != nullptr) {
        size_t dst_size = dst_wstride_ * model_height_ * 3;  // RGB888，使用 wstride
        memset(dst_virt_addr_, 0, dst_size);  // 黑色填充
        LOG_DEBUG("Background filled with black (0)");
    } else {
        LOG_WARN("dst_virt_addr_ is nullptr, cannot fill background!");
    }

    // 4. 设置源和目标矩形（参考官方 image_utils.c:536-563）
    im_rect srect;
    srect.x = 0;
    srect.y = 0;
    srect.width = src_width;
    srect.height = src_height;

    im_rect drect;
    drect.x = dst_box.x;
    drect.y = dst_box.y;
    drect.width = dst_box.width;
    drect.height = dst_box.height;

    im_rect prect = {0, 0, 0, 0};

    // 5. 执行 RGA 处理（添加同步和完整检查）
    rga_buffer_t pat = {};
    int usage = 0;
    usage |= IM_SYNC;  // 同步执行

    IM_STATUS status = improcess(rga_buf_src, rga_buf_dst, pat, srect, drect, prect, usage);
    if (status != IM_STATUS_SUCCESS) {
        LOG_ERROR("improcess failed: STATUS={}, src={}x{}, dst_box=({},{}-{}x{}), error={}",
                  static_cast<int>(status), src_width, src_height,
                  drect.x, drect.y, drect.width, drect.height, imStrError(status));
        return false;
    }

    return true;
}

} // namespace nvr::detection
