//
// Created by Claude on 2026/4/19.
// 预缓存帧缓冲区 - 用于智能录制的预缓存功能
//

#ifndef NVR_FRAME_BUFFER_H
#define NVR_FRAME_BUFFER_H

#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
}

namespace nvr {

// 缓存的帧信息
struct CachedFrame {
    AVPacket* packet;                      // AVPacket（需要释放）
    int64_t pts;                           // PTS 时间戳
    int64_t dts;                           // DTS 时间戳
    uint64_t sequence_number;              // 序列号
    bool is_key_frame;                     // 是否为关键帧

    CachedFrame()
        : packet(nullptr)
        , pts(0)
        , dts(0)
        , sequence_number(0)
        , is_key_frame(false) {}

    // 移动构造函数
    CachedFrame(CachedFrame&& other) noexcept
        : packet(other.packet)
        , pts(other.pts)
        , dts(other.dts)
        , sequence_number(other.sequence_number)
        , is_key_frame(other.is_key_frame) {
        other.packet = nullptr;
    }

    // 移动赋值运算符
    CachedFrame& operator=(CachedFrame&& other) noexcept {
        if (this != &other) {
            // 释放现有 packet
            if (packet) {
                av_packet_free(&packet);
            }

            packet = other.packet;
            pts = other.pts;
            dts = other.dts;
            sequence_number = other.sequence_number;
            is_key_frame = other.is_key_frame;

            other.packet = nullptr;
        }
        return *this;
    }

    // 禁止拷贝
    CachedFrame(const CachedFrame&) = delete;
    CachedFrame& operator=(const CachedFrame&) = delete;

    ~CachedFrame() {
        if (packet) {
            av_packet_free(&packet);
        }
    }
};

// 循环帧缓冲区
class FrameBuffer {
public:
    explicit FrameBuffer(int max_duration_seconds = 5);
    ~FrameBuffer();

    // 禁止拷贝
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    // 添加帧到缓冲区
    bool addFrame(AVPacket* packet, int64_t pts, int64_t dts, bool is_key_frame);

    // 获取所有缓存的帧（用于写入文件）
    std::vector<CachedFrame> getAllFrames() const;

    // 获取从指定 PTS 开始的所有帧
    std::vector<CachedFrame> getFramesSince(int64_t start_pts) const;

    // 获取最近的 N 秒帧
    std::vector<CachedFrame> getRecentFrames(int duration_seconds) const;

    // 清空缓冲区
    void clear();

    // 获取缓冲区大小（秒）
    double getDurationSeconds() const;

    // 获取帧数量
    size_t getFrameCount() const;

    // 获取第一个帧的 PTS
    int64_t getFirstPTS() const;

    // 获取最后一个帧的 PTS
    int64_t getLastPTS() const;

    // 设置时间基准（用于计算时长）
    void setTimeBase(AVRational time_base) { time_base_ = time_base; }

    // 获取时间基准
    AVRational getTimeBase() const { return time_base_; }

private:
    // 清理旧帧
    void cleanupOldFrames();

    // 计算 PTS 差值对应的秒数
    double ptsToSeconds(int64_t pts_diff) const;

    mutable std::mutex mutex_;
    std::deque<CachedFrame> frames_;
    int max_duration_seconds_;
    AVRational time_base_;
    uint64_t next_sequence_number_;
};

// 帧缓冲区写入器（用于将缓存的帧写入文件）
class FrameBufferWriter {
public:
    explicit FrameBufferWriter(AVFormatContext* output_ctx, int video_stream_index);
    ~FrameBufferWriter();

    // 写入缓存的帧
    bool writeFrames(const std::vector<CachedFrame>& frames);

    // 设置时间基准
    void setTimeBase(AVRational time_base);

    // 设置 PTS 偏移量（用于调整时间戳）
    void setPTSOffset(int64_t pts_offset, int64_t dts_offset);

private:
    AVFormatContext* output_ctx_;
    int video_stream_index_;
    AVRational time_base_;
    int64_t pts_offset_;
    int64_t dts_offset_;
    bool offsets_set_;
};

} // namespace nvr

#endif // NVR_FRAME_BUFFER_H
