//
// Created by Claude on 2026/4/19.
// 预缓存帧缓冲区实现
//

#include "frame_buffer.h"
#include "log.h"
#include <algorithm>

namespace nvr {

// ============================================================================
// FrameBuffer 实现
// ============================================================================

FrameBuffer::FrameBuffer(int max_duration_seconds)
    : max_duration_seconds_(max_duration_seconds)
    , time_base_{1, 90000}  // 默认时间基准
    , next_sequence_number_(0)
{
    frames_.clear();
}

FrameBuffer::~FrameBuffer() {
    clear();
}

bool FrameBuffer::addFrame(AVPacket* packet, int64_t pts, int64_t dts, bool is_key_frame) {
    if (!packet) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // 创建新的缓存帧
    CachedFrame cached_frame;
    cached_frame.packet = av_packet_clone(packet);
    if (!cached_frame.packet) {
        LOG_ERROR("Failed to clone packet for frame buffer");
        return false;
    }

    cached_frame.pts = pts;
    cached_frame.dts = dts;
    cached_frame.sequence_number = next_sequence_number_++;
    cached_frame.is_key_frame = is_key_frame;

    // 添加到缓冲区
    frames_.push_back(std::move(cached_frame));

    // 清理旧帧
    cleanupOldFrames();

    return true;
}

std::vector<CachedFrame> FrameBuffer::getAllFrames() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<CachedFrame> result;
    result.reserve(frames_.size());

    for (const auto& frame : frames_) {
        // 深拷贝 packet
        CachedFrame new_frame;
        new_frame.packet = av_packet_clone(frame.packet);
        new_frame.pts = frame.pts;
        new_frame.dts = frame.dts;
        new_frame.sequence_number = frame.sequence_number;
        new_frame.is_key_frame = frame.is_key_frame;

        result.push_back(std::move(new_frame));
    }

    return result;
}

std::vector<CachedFrame> FrameBuffer::getFramesSince(int64_t start_pts) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<CachedFrame> result;

    // 查找起始位置
    auto start_it = std::find_if(frames_.begin(), frames_.end(),
        [start_pts](const CachedFrame& frame) {
            return frame.pts >= start_pts;
        });

    if (start_it == frames_.end()) {
        return result;  // 没有找到符合条件的帧
    }

    // 复制从起始位置到结尾的所有帧
    result.reserve(std::distance(start_it, frames_.end()));
    for (auto it = start_it; it != frames_.end(); ++it) {
        CachedFrame new_frame;
        new_frame.packet = av_packet_clone(it->packet);
        new_frame.pts = it->pts;
        new_frame.dts = it->dts;
        new_frame.sequence_number = it->sequence_number;
        new_frame.is_key_frame = it->is_key_frame;

        result.push_back(std::move(new_frame));
    }

    return result;
}

std::vector<CachedFrame> FrameBuffer::getRecentFrames(int duration_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frames_.empty()) {
        return {};
    }

    // 计算截止 PTS
    int64_t last_pts = frames_.back().pts;
    int64_t pts_threshold = last_pts - static_cast<int64_t>(duration_seconds * time_base_.den / time_base_.num);

    // 查找起始位置
    auto start_it = std::find_if(frames_.begin(), frames_.end(),
        [pts_threshold](const CachedFrame& frame) {
            return frame.pts >= pts_threshold;
        });

    if (start_it == frames_.end()) {
        return getAllFrames();
    }

    // 复制从起始位置到结尾的所有帧
    std::vector<CachedFrame> result;
    result.reserve(std::distance(start_it, frames_.end()));
    for (auto it = start_it; it != frames_.end(); ++it) {
        CachedFrame new_frame;
        new_frame.packet = av_packet_clone(it->packet);
        new_frame.pts = it->pts;
        new_frame.dts = it->dts;
        new_frame.sequence_number = it->sequence_number;
        new_frame.is_key_frame = it->is_key_frame;

        result.push_back(std::move(new_frame));
    }

    return result;
}

void FrameBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
}

double FrameBuffer::getDurationSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (frames_.size() < 2) {
        return 0.0;
    }

    int64_t pts_diff = frames_.back().pts - frames_.front().pts;
    return ptsToSeconds(pts_diff);
}

size_t FrameBuffer::getFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
}

int64_t FrameBuffer::getFirstPTS() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.empty() ? 0 : frames_.front().pts;
}

int64_t FrameBuffer::getLastPTS() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_.empty() ? 0 : frames_.back().pts;
}

void FrameBuffer::cleanupOldFrames() {
    if (max_duration_seconds_ <= 0 || frames_.size() < 2) {
        return;
    }

    // 计算最大允许的 PTS 差值
    int64_t max_pts_diff = static_cast<int64_t>(max_duration_seconds_ * time_base_.den / time_base_.num);

    // 找到第一个应该保留的帧
    int64_t last_pts = frames_.back().pts;
    auto erase_end = frames_.begin();

    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (last_pts - it->pts <= max_pts_diff) {
            erase_end = it;
            break;
        }
    }

    // 删除旧帧
    if (erase_end != frames_.begin()) {
        size_t erase_count = std::distance(frames_.begin(), erase_end);
        spdlog::trace("Cleaning up {} old frames from buffer", erase_count);
        frames_.erase(frames_.begin(), erase_end);
    }
}

double FrameBuffer::ptsToSeconds(int64_t pts_diff) const {
    if (time_base_.num == 0) {
        return 0.0;
    }
    return static_cast<double>(pts_diff) * time_base_.num / time_base_.den;
}

// ============================================================================
// FrameBufferWriter 实现
// ============================================================================

FrameBufferWriter::FrameBufferWriter(AVFormatContext* output_ctx, int video_stream_index)
    : output_ctx_(output_ctx)
    , video_stream_index_(video_stream_index)
    , time_base_{1, 90000}
    , pts_offset_(0)
    , dts_offset_(0)
    , offsets_set_(false)
{
}

FrameBufferWriter::~FrameBufferWriter() = default;

bool FrameBufferWriter::writeFrames(const std::vector<CachedFrame>& frames) {
    if (!output_ctx_) {
        LOG_ERROR("FrameBufferWriter: output context is null");
        return false;
    }

    for (const auto& cached_frame : frames) {
        if (!cached_frame.packet) {
            continue;
        }

        // 克隆 packet（避免修改原始数据）
        AVPacket* pkt = av_packet_clone(cached_frame.packet);
        if (!pkt) {
            LOG_ERROR("Failed to clone packet for writing");
            continue;
        }

        // 设置流索引
        pkt->stream_index = video_stream_index_;

        // 调整时间戳
        if (offsets_set_) {
            pkt->pts -= pts_offset_;
            pkt->dts -= dts_offset_;
        }

        // 转换时间基准到输出流
        AVStream* out_stream = output_ctx_->streams[video_stream_index_];
        av_packet_rescale_ts(pkt, time_base_, out_stream->time_base);

        // 写入包
        int ret = av_interleaved_write_frame(output_ctx_, pkt);
        av_packet_free(&pkt);

        if (ret < 0) {
            char err_buf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, err_buf, sizeof(err_buf));
            LOG_ERROR("Failed to write frame: {}", err_buf);
            return false;
        }
    }

    return true;
}

void FrameBufferWriter::setTimeBase(AVRational time_base) {
    time_base_ = time_base;
}

void FrameBufferWriter::setPTSOffset(int64_t pts_offset, int64_t dts_offset) {
    pts_offset_ = pts_offset;
    dts_offset_ = dts_offset;
    offsets_set_ = true;
}

} // namespace nvr
