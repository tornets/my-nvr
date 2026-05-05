//
// Created by Claude on 2026/4/19.
// 智能录制管理器实现
//

#include "smart_recording_manager.h"
#include "config_loader.h"
#include "log.h"
#include <chrono>

namespace nvr {

// ============================================================================
// SmartRecordingManager 实现
// ============================================================================

SmartRecordingManager::SmartRecordingManager(
    const SmartRecordingConfig& config,
    const std::string& stream_id,
    detection::DetectionPool& detection_pool)
    : config_(config)
    , stream_id_(stream_id)
    , detection_pool_(detection_pool)
    , initialized_(false)
    , running_(false)
    , current_state_(SmartRecordingState::IDLE)
    , segment_start_pts_(0)
    , last_detection_pts_(0)
    , time_base_{1, 90000}
    , output_ctx_(nullptr)
    , video_stream_index_(-1)
    , keyframe_count_(0)
    , detection_count_(0)
    , player_detected_count_(0)
    , should_write_(false)
    , logger_(spdlog::get("nvr") ? spdlog::get("nvr") : spdlog::default_logger())
{
    player_last_seen_ = std::chrono::steady_clock::now();
    recording_stop_time_ = std::chrono::steady_clock::time_point::max();
}

SmartRecordingManager::~SmartRecordingManager() {
    shutdown();
}

bool SmartRecordingManager::initialize() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (initialized_) {
        LOG_WARN("SmartRecordingManager already initialized for stream: {}", stream_id_);
        return true;
    }

    LOG_INFO("Initializing SmartRecordingManager for stream: {}", stream_id_);

    // 检查配置
    if (!config_.enabled) {
        LOG_INFO("Smart recording disabled for stream: {}", stream_id_);
        return true;
    }

    if (!config_.rknn.enabled) {
        LOG_WARN("RKNN detection disabled, smart recording will not work properly");
        return false;
    }

    // 创建帧缓冲区
    frame_buffer_ = std::make_unique<FrameBuffer>(config_.prebuffer_duration_seconds);

    // 创建检测结果缓存
    detection_cache_ = std::make_unique<detection::DetectionResultCache>(
        config_.segment_duration_seconds);

    // 创建分段决策器
    segment_decision_ = std::make_unique<detection::SmartSegmentDecision>(config_);

    initialized_ = true;
    LOG_INFO("SmartRecordingManager initialized successfully for stream: {}", stream_id_);

    return true;
}

void SmartRecordingManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) {
            return;
        }
        LOG_INFO("Shutting down SmartRecordingManager for stream: {}", stream_id_);
        if (!running_) {
            initialized_ = false;
            return;
        }
        running_ = false;
    }

    // 在锁外停止检测线程（避免死锁）
    queue_cv_.notify_all();
    if (detection_thread_.joinable()) {
        detection_thread_.join();
    }

    // 清空队列
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        while (!detection_queue_.empty()) {
            auto& task = detection_queue_.front();
            if (task.frame) {
                av_frame_free(&task.frame);
            }
            detection_queue_.pop();
        }
    }

    // 清理资源
    frame_buffer_.reset();
    detection_cache_.reset();
    segment_decision_.reset();

    initialized_ = false;
    LOG_INFO("SmartRecordingManager shut down for stream: {}", stream_id_);
}

bool SmartRecordingManager::start() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!initialized_) {
        LOG_ERROR("Cannot start: SmartRecordingManager not initialized");
        return false;
    }

    if (running_) {
        LOG_WARN("SmartRecordingManager already running for stream: {}", stream_id_);
        return true;
    }

    LOG_INFO("Starting SmartRecordingManager for stream: {}", stream_id_);

    running_ = true;
    current_state_ = SmartRecordingState::IDLE;
    keyframe_count_ = 0;
    detection_count_ = 0;
    player_detected_count_ = 0;

    // 启动检测线程
    detection_thread_ = std::thread(&SmartRecordingManager::detectionWorkerThread, this);

    LOG_INFO("SmartRecordingManager started successfully for stream: {}", stream_id_);

    return true;
}

void SmartRecordingManager::stop() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
    }

    LOG_INFO("Stopping SmartRecordingManager for stream: {}", stream_id_);

    // 在锁外停止检测线程（避免死锁：handleDetectionResult 也获取 state_mutex_）
    queue_cv_.notify_all();
    if (detection_thread_.joinable()) {
        detection_thread_.join();
    }

    // 清空队列
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        while (!detection_queue_.empty()) {
            auto& task = detection_queue_.front();
            if (task.frame) {
                av_frame_free(&task.frame);
            }
            detection_queue_.pop();
        }
    }

    LOG_INFO("SmartRecordingManager stopped for stream: {}", stream_id_);
}

bool SmartRecordingManager::processVideoFrame(
    AVPacket* packet,
    AVFrame* decoded_frame,
    int64_t pts,
    int64_t dts,
    bool is_key_frame) {

    if (!running_) {
        return false;
    }

    // 添加到帧缓冲区（预缓存）
    if (frame_buffer_) {
        frame_buffer_->addFrame(packet, pts, dts, is_key_frame);
    }

    // 统计关键帧
    if (is_key_frame) {
        keyframe_count_++;
    }

    // 检查是否需要运行检测
    if (shouldRunDetection(is_key_frame, pts) && decoded_frame) {
        // 更新上次检测 PTS
        last_detection_pts_ = pts;

        // 将解码后的帧送入检测队列
        DetectionTask task;
        task.pts = pts;
        task.dts = dts;
        task.is_key_frame = is_key_frame;

        // 引用计数传递帧，检测线程完成后释放
        task.frame = av_frame_clone(decoded_frame);
        if (task.frame) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            // 队列最多保留 2 个待处理帧，丢弃旧的
            while (detection_queue_.size() >= 2) {
                auto& old = detection_queue_.front();
                if (old.frame) av_frame_free(&old.frame);
                detection_queue_.pop();
            }
            detection_queue_.push(std::move(task));
            queue_cv_.notify_one();
        }
    }

    return true;
}

void SmartRecordingManager::detectionWorkerThread() {
    LOG_DEBUG("Detection worker thread started for stream: {}", stream_id_);

    while (running_) {
        DetectionTask task;

        // 从队列获取任务
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !running_ || !detection_queue_.empty();
            });

            if (!running_) {
                break;
            }

            if (detection_queue_.empty()) {
                continue;
            }

            task = std::move(detection_queue_.front());
            detection_queue_.pop();
        }

        // 执行检测
        if (!task.frame) {
            continue;
        }

#if DUMP_DECTECT_IMAGE
        int frame_idx = debug_decode_frame_index_++;
        saveDecodedFrame(task.frame, frame_idx);
#endif

        // 提交到 NPU 推理池（同步等待结果）
        auto pool_result = detection_pool_.detect(task.frame);

        if (pool_result.success) {
            detection_count_++;
            const auto& result = pool_result.detection;
            LOG_DEBUG("[{}] Detection #{}: {} boxes, has_player={}, player_conf={:.2f}, time={:.1f}ms",
                           stream_id_, detection_count_.load(), result.boxes.size(),
                           result.has_player, result.player_confidence,
                           result.processing_time_ms);
            handleDetectionResult(result);
#if DUMP_DECTECT_IMAGE
            saveDetectionImage(pool_result, frame_idx);
#endif
        } else {
            LOG_WARN("Detection failed for frame pts={}", task.pts);
        }

        // 释放帧
        if (task.frame) {
            av_frame_free(&task.frame);
        }  // clone 的帧需要 av_frame_free
    }

    LOG_DEBUG("Detection worker thread stopped for stream: {}", stream_id_);
}

void SmartRecordingManager::handleDetectionResult(const detection::DetectionResult& result) {
    std::lock_guard<std::mutex> lock(state_mutex_);

    // 添加到缓存
    if (detection_cache_) {
        detection_cache_->addResult(result, result.frame_pts);
    }

    // 统计
    if (result.has_player) {
        player_detected_count_++;
        player_last_seen_ = std::chrono::steady_clock::now();
    }

    // 状态机处理
    switch (current_state_) {
        case SmartRecordingState::IDLE:
            if (result.has_player) {
                LOG_INFO("Player detected, starting recording (stream: {})", stream_id_);
                should_write_ = true;
                transitionTo(SmartRecordingState::RECORDING);
            }
            break;

        case SmartRecordingState::RECORDING:
            if (!result.has_player) {
                // 玩家消失，进入延迟停止状态
                int delay = config_.post_recording_delay_seconds;
                recording_stop_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
                LOG_INFO("Player disappeared, post-recording delay {}s (stream: {})", delay, stream_id_);
                transitionTo(SmartRecordingState::POST_RECORDING);
            }
            break;

        case SmartRecordingState::POST_RECORDING:
            if (result.has_player) {
                // 玩家重新出现，继续录制
                LOG_INFO("Player reappeared, resuming recording (stream: {})", stream_id_);
                recording_stop_time_ = std::chrono::steady_clock::time_point::max();
                transitionTo(SmartRecordingState::RECORDING);
            }
            break;
    }
}

void SmartRecordingManager::transitionTo(SmartRecordingState new_state) {
    if (current_state_ == new_state) {
        return;
    }

    LOG_INFO("State transition: {} -> {} (stream: {})",
                 getStateName(current_state_), getStateName(new_state), stream_id_);

    // 进入 IDLE 状态时停止写入
    if (new_state == SmartRecordingState::IDLE) {
        should_write_ = false;
    }

    current_state_ = new_state;
}

bool SmartRecordingManager::shouldRunDetection(bool is_key_frame, int64_t pts) {
    if (!config_.rknn.enabled) {
        return false;
    }

    switch (config_.rknn.detection_mode) {
    case DetectionMode::Keyframe: {
        if (!is_key_frame) {
            return false;  // 只检测关键帧
        }
        // 检查检测间隔
        int interval = config_.rknn.detection_interval_keyframes;
        if (interval <= 0) {
            return false;
        }
        return (keyframe_count_ % interval) == 0;
    }

    case DetectionMode::Sampled: {
        // 关键帧始终检测（确保不漏），非关键帧按 PTS 时间间隔
        if (is_key_frame) {
            return true;
        }
        float interval = config_.rknn.detection_interval_seconds;
        if (interval <= 0) {
            return false;
        }
        AVRational tb = time_base_;
        int64_t interval_pts = static_cast<int64_t>(interval * tb.den / tb.num);
        return (pts - last_detection_pts_) >= interval_pts;
    }

    case DetectionMode::Realtime:
        return true;  // 每帧都检测

    default:
        return false;
    }
}

bool SmartRecordingManager::shouldDecodeForDetection(bool is_key_frame, int64_t pts) {
    if (!config_.rknn.enabled) {
        return false;
    }

    switch (config_.rknn.detection_mode) {
    case DetectionMode::Keyframe:
        return is_key_frame;  // 只解码关键帧

    case DetectionMode::Sampled:
        // 关键帧始终解码，非关键帧按时间间隔判断
        if (is_key_frame) {
            return true;
        }
        // 对于非关键帧，检查是否到了检测间隔
        // 如果间隔到了，返回 true 让解码器解码，然后 shouldRunDetection 会返回 true 进行检测
        {
            float interval = config_.rknn.detection_interval_seconds;
            if (interval <= 0) {
                return false;
            }
            AVRational tb = time_base_;
            int64_t interval_pts = static_cast<int64_t>(interval * tb.den / tb.num);
            // 注意：这里需要修改 last_detection_pts_，所以不能是 const
            // 暂时移除 const 限定，或者在调用方处理
            return (pts - last_detection_pts_) >= interval_pts;
        }

    case DetectionMode::Realtime:
        return true;  // 每帧都解码

    default:
        return false;
    }
}

const char* SmartRecordingManager::getStateName(SmartRecordingState state) {
    switch (state) {
        case SmartRecordingState::IDLE:           return "IDLE";
        case SmartRecordingState::RECORDING:      return "RECORDING";
        case SmartRecordingState::POST_RECORDING: return "POST_RECORDING";
        default:                                   return "UNKNOWN";
    }
}

bool SmartRecordingManager::shouldStartNewSegment() const {
    return false;  // 智能录制模式下由门控状态机管理
}

std::vector<CachedFrame> SmartRecordingManager::getCachedFrames() const {
    if (frame_buffer_) {
        return frame_buffer_->getAllFrames();
    }
    return {};
}

std::vector<CachedFrame> SmartRecordingManager::getFramesSince(int64_t start_pts) const {
    if (frame_buffer_) {
        return frame_buffer_->getFramesSince(start_pts);
    }
    return {};
}

bool SmartRecordingManager::shouldWritePacket() const {
    return should_write_.load(std::memory_order_relaxed);
}

std::vector<CachedFrame> SmartRecordingManager::getPrebufferFrames() const {
    if (!frame_buffer_) {
        return {};
    }

    auto all_frames = frame_buffer_->getAllFrames();

    // 找到第一个关键帧作为起始点（必须从关键帧开始才能正确解码）
    size_t start_idx = 0;
    for (size_t i = 0; i < all_frames.size(); i++) {
        if (all_frames[i].is_key_frame) {
            start_idx = i;
            break;  // 找到第一个关键帧就停止
        }
    }

    // 如果没有找到关键帧，返回空（避免写入无法解码的帧）
    if (start_idx == 0 && !all_frames.empty() && !all_frames[0].is_key_frame) {
        LOG_WARN("No keyframe found in prebuffer, skipping prebuffer write");
        return {};
    }

    // 从第一个关键帧开始返回
    std::vector<CachedFrame> result;
    for (size_t i = start_idx; i < all_frames.size(); i++) {
        result.push_back(std::move(all_frames[i]));
    }
    return result;
}

bool SmartRecordingManager::shouldStopRecording() {
    if (current_state_ != SmartRecordingState::POST_RECORDING) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    if (now >= recording_stop_time_) {
        should_write_ = false;
        transitionTo(SmartRecordingState::IDLE);
        return true;
    }
    return false;
}

void SmartRecordingManager::clearCache() {
    if (frame_buffer_) {
        frame_buffer_->clear();
    }
    if (detection_cache_) {
        detection_cache_->clear();
    }
}

void SmartRecordingManager::setOutputContext(AVFormatContext* output_ctx, int video_stream_index) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    output_ctx_ = output_ctx;
    video_stream_index_ = video_stream_index;
}

void SmartRecordingManager::setTimeBase(AVRational time_base) {
    time_base_ = time_base;
    if (frame_buffer_) {
        frame_buffer_->setTimeBase(time_base);
    }
}

DetectionStats SmartRecordingManager::getDetectionStats() const {
    return DetectionStats();
}

void SmartRecordingManager::resetDetectionStats() {
    keyframe_count_ = 0;
    detection_count_ = 0;
    player_detected_count_ = 0;
}

void SmartRecordingManager::setSegmentStartTime(int64_t pts) {
    segment_start_pts_ = pts;
}

double SmartRecordingManager::getSegmentDurationSeconds() const {
    if (!detection_cache_ || detection_cache_->empty()) {
        return 0.0;
    }

    int64_t last_pts = detection_cache_->getLastPTS();
    if (segment_start_pts_ == 0 || last_pts < segment_start_pts_) {
        return 0.0;
    }

    int64_t pts_diff = last_pts - segment_start_pts_;
    return static_cast<double>(pts_diff) * time_base_.num / time_base_.den;
}

bool SmartRecordingManager::writeCachedFrames(const std::vector<CachedFrame>& frames) {
    std::lock_guard<std::mutex> lock(output_mutex_);

    if (!output_ctx_ || video_stream_index_ < 0) {
        LOG_ERROR("Output context not set, cannot write cached frames");
        return false;
    }

    FrameBufferWriter writer(output_ctx_, video_stream_index_);
    writer.setTimeBase(time_base_);

    if (segment_start_pts_ > 0) {
        writer.setPTSOffset(segment_start_pts_, segment_start_pts_);
    }

    return writer.writeFrames(frames);
}

void SmartRecordingManager::cleanupOldDetections() {
    if (detection_cache_) {
        detection_cache_->cleanOldEntries(config_.segment_duration_seconds);
    }
}

} // namespace nvr

// ============================================================================
// DUMP_DECTECT_IMAGE 调试图像导出
// ============================================================================
#if DUMP_DECTECT_IMAGE

#include <filesystem>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

namespace {

// 5×7 位图字体（每个字符 7 字节，每字节低 5 位为一行）
struct Glyph5x7 { char ch; uint8_t rows[7]; };

static const Glyph5x7 FONT[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
    {'3',{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x06,0x06}},
    {':',{0x00,0x00,0x06,0x00,0x06,0x00,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x1F,0x00,0x00}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'a',{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}},
    {'e',{0x00,0x00,0x0E,0x11,0x1E,0x10,0x0E}},
    {'l',{0x08,0x08,0x08,0x08,0x08,0x08,0x08}},
    {'r',{0x00,0x00,0x0E,0x11,0x1E,0x10,0x10}},
    {'y',{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}},
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
};

const Glyph5x7* findGlyph(char c) {
    for (const auto& g : FONT) {
        if (g.ch == c) return &g;
    }
    return nullptr;
}

// 以 scale 倍率绘制文字（scale=2 即 10×14 像素/字符）
void drawText(std::vector<uint8_t>& rgb, int img_w, int img_h,
              const std::string& text, int x, int y, int scale,
              uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    for (char c : text) {
        const Glyph5x7* gl = findGlyph(c);
        if (!gl) { cx += (6 * scale); continue; }
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (gl->rows[row] & (0x10 >> col)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = cx + col * scale + sx;
                            int py = y + row * scale + sy;
                            if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                                int idx = (py * img_w + px) * 3;
                                rgb[idx] = r;
                                rgb[idx+1] = g;
                                rgb[idx+2] = b;
                            }
                        }
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

// 测量文字像素宽度
int measureText(const std::string& text, int scale) {
    return static_cast<int>(text.size()) * 6 * scale;
}

void drawRect(std::vector<uint8_t>& rgb, int img_w, int img_h,
              int rx, int ry, int rw, int rh, int thickness,
              uint8_t r, uint8_t g, uint8_t b) {
    for (int t = 0; t < thickness; t++) {
        // 上边
        for (int x = rx; x < rx + rw && x < img_w; x++) {
            int py = ry + t;
            if (py >= 0 && py < img_h && x >= 0) {
                int idx = (py * img_w + x) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
        // 下边
        for (int x = rx; x < rx + rw && x < img_w; x++) {
            int py = ry + rh - 1 - t;
            if (py >= 0 && py < img_h && x >= 0) {
                int idx = (py * img_w + x) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
        // 左边
        for (int y = ry; y < ry + rh && y < img_h; y++) {
            int px = rx + t;
            if (px >= 0 && px < img_w && y >= 0) {
                int idx = (y * img_w + px) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
        // 右边
        for (int y = ry; y < ry + rh && y < img_h; y++) {
            int px = rx + rw - 1 - t;
            if (px >= 0 && px < img_w && y >= 0) {
                int idx = (y * img_w + px) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
    }
}

// 填充矩形
void fillRect(std::vector<uint8_t>& rgb, int img_w, int img_h,
              int rx, int ry, int rw, int rh,
              uint8_t r, uint8_t g, uint8_t b) {
    for (int y = ry; y < ry + rh && y < img_h; y++) {
        if (y < 0) continue;
        for (int x = rx; x < rx + rw && x < img_w; x++) {
            if (x < 0) continue;
            int idx = (y * img_w + x) * 3;
            rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
        }
    }
}

bool saveJpeg(const std::string& path, const uint8_t* rgb_data, int width, int height) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return false;

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = {1, 25};
    ctx->qmin = 2;
    ctx->qmax = 10;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return false;
    }

    SwsContext* sws = sws_getContext(width, height, AV_PIX_FMT_RGB24,
                                      width, height, AV_PIX_FMT_YUVJ420P,
                                      SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
    if (!sws) {
        avcodec_free_context(&ctx);
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUVJ420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);

    const uint8_t* src_data[1] = { rgb_data };
    int src_stride[1] = { width * 3 };
    sws_scale(sws, src_data, src_stride, 0, height, frame->data, frame->linesize);

    avcodec_send_frame(ctx, frame);

    bool ok = false;
    AVPacket* pkt = av_packet_alloc();
    if (avcodec_receive_packet(ctx, pkt) >= 0) {
        FILE* f = fopen(path.c_str(), "wb");
        if (f) {
            fwrite(pkt->data, 1, pkt->size, f);
            fclose(f);
            ok = true;
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);
    return ok;
}

} // anonymous namespace

namespace nvr {

// 调试图像旋转校正（逆时针度数）
// 当摄像头传感器旋转导致调试图像方向不对时设置
// 0=不旋转，90=逆时针90度（修正顺时针90度的摄像头）
// 对方形图像（640x640）旋转不改变尺寸
constexpr int kDebugImageRotation = 0;  // RGA 修复后不再需要旋转补偿

// 逆时针旋转 RGB 图像 90 度：pixel(x,y) → pixel(y, W-1-x)
void rotateCCW90(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, int w, int h) {
    dst.resize(src.size());
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int nx = y;
            int ny = w - 1 - x;
            int si = (y * w + x) * 3;
            int di = (ny * w + nx) * 3;
            dst[di]     = src[si];
            dst[di + 1] = src[si + 1];
            dst[di + 2] = src[si + 2];
        }
    }
}

} // anonymous namespace

namespace nvr {

void SmartRecordingManager::saveDetectionImage(const detection::PoolDetectionResult& pool_result, int detect_count) {
    if (debug_output_dir_.empty()) return;

    const auto& rgb_src = pool_result.debug_rgb;
    int w = pool_result.debug_w;
    int h = pool_result.debug_h;
    if (rgb_src.empty() || w <= 0 || h <= 0) return;

    // 先旋转 RGB 数据（如果需要）
    std::vector<uint8_t> img;
    bool rotated = (kDebugImageRotation == 90 || kDebugImageRotation == 270);
    if (kDebugImageRotation == 90) {
        rotateCCW90(rgb_src, img, w, h);
    } else {
        img = rgb_src;
    }

    const int font_scale = 2;          // 字体缩放 2x（10×14 px/字符）
    const int box_thickness = 3;       // 框线粗细
    const int label_pad = 2;           // 标签内边距

    // 画图例（左上角）
    {
        const int legend_x = 4;
        int legend_y = 4;
        const int legend_font_scale = 2;
        const int box_size = 10;

        // Player 图例（绿色方块 + 文字）
        fillRect(img, w, h, legend_x, legend_y, box_size, box_size, 0, 255, 0);
        drawRect(img, w, h, legend_x, legend_y, box_size, box_size, 1, 200, 200, 200);
        drawText(img, w, h, " Player", legend_x + box_size + 2, legend_y + 1, legend_font_scale, 255, 255, 255);

        // NPC 图例（红色方块 + 文字）
        int npc_y = legend_y + box_size + 4 + 2;
        fillRect(img, w, h, legend_x, npc_y, box_size, box_size, 255, 0, 0);
        drawRect(img, w, h, legend_x, npc_y, box_size, box_size, 1, 200, 200, 200);
        drawText(img, w, h, " NPC", legend_x + box_size + 2, npc_y + 1, legend_font_scale, 255, 255, 255);
    }

    // 画 box 和 label（坐标需要随旋转变换）
    for (const auto& box : pool_result.detection.boxes) {
        int bx = static_cast<int>(box.x);
        int by = static_cast<int>(box.y);
        int bw = static_cast<int>(box.width);
        int bh = static_cast<int>(box.height);

        // 旋转坐标：逆时针90度 (bx,by,bw,bh) → (by, W-1-bx-bw, bh, bw)
        if (kDebugImageRotation == 90) {
            int new_bx = by;
            int new_by = w - 1 - bx - bw;
            int new_bw = bh;
            int new_bh = bw;
            bx = new_bx; by = new_by; bw = new_bw; bh = new_bh;
        }

        bool is_player = (box.class_id == config_.rknn.player_class_id);
        bool is_npc = (box.class_id == config_.rknn.npc_class_id);
        if (!is_player && !is_npc) continue;

        uint8_t cr = is_player ? 0 : 255;
        uint8_t cg = is_player ? 255 : 0;
        uint8_t cb = 0;

        drawRect(img, w, h, bx, by, bw, bh, box_thickness, cr, cg, cb);

        // label: "P:0.57" 或 "N:0.42"
        char label[16];
        snprintf(label, sizeof(label), "%c:%.2f", is_player ? 'P' : 'N', box.confidence);

        int label_w = measureText(label, font_scale) + label_pad * 2;
        int label_h = 7 * font_scale + label_pad * 2;
        int label_y = by - label_h - 2;
        if (label_y < 0) label_y = by + bh + 2;

        fillRect(img, w, h, bx, label_y, label_w, label_h, 0, 0, 0);
        drawText(img, w, h, label, bx + label_pad, label_y + label_pad, font_scale, cr, cg, cb);
    }

    // 构造输出路径: {output_dir}/debug/detect/{date}/{stream_id}_{count}_{player}P_{npc}N.jpg
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&now_t);
    char date_str[16];
    std::strftime(date_str, sizeof(date_str), "%Y%m%d", tm);

    int player_count = 0, npc_count = 0;
    for (const auto& box : pool_result.detection.boxes) {
        if (box.class_id == config_.rknn.player_class_id) player_count++;
        else if (box.class_id == config_.rknn.npc_class_id) npc_count++;
    }

    char fname[256];
    snprintf(fname, sizeof(fname), "%s_%d_%dP_%dN.jpg",
             stream_id_.c_str(), detect_count,
             player_count, npc_count);

    std::filesystem::path dir = std::filesystem::path(debug_output_dir_) / "debug" / "detect" / date_str;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::string path = (dir / fname).string();
    saveJpeg(path, img.data(), w, h);
}

void SmartRecordingManager::saveDecodedFrame(AVFrame* frame, int frame_idx) {
    if (!frame || debug_output_dir_.empty()) return;

    int width = frame->width;
    int height = frame->height;
    if (width <= 0 || height <= 0) return;

    // 创建日期目录: {output_dir}/debug/decode/{date}
    auto now = std::chrono::system_clock::now();
    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm* tm = std::localtime(&now_t);
    char date_str[16];
    std::strftime(date_str, sizeof(date_str), "%Y%m%d", tm);

    std::filesystem::path dir = std::filesystem::path(debug_output_dir_) / "debug" / "decode" / date_str;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    char fname[256];
    snprintf(fname, sizeof(fname), "%s_%d.jpg", stream_id_.c_str(), frame_idx);
    std::string path = (dir / fname).string();

    // DRM_PRIME → CPU 帧
    AVFrame* cpu_frame = av_frame_alloc();
    int ret = av_hwframe_transfer_data(cpu_frame, frame, 0);
    if (ret < 0) {
        LOG_DEBUG("saveDecodedFrame: hwframe transfer failed ({})", ret);
        av_frame_free(&cpu_frame);
        return;
    }
    av_frame_copy_props(cpu_frame, frame);
    cpu_frame->width = width;
    cpu_frame->height = height;

    // 转换为 RGB
    SwsContext* sws = sws_getContext(
        width, height, static_cast<AVPixelFormat>(cpu_frame->format),
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
    if (!sws) {
        av_frame_free(&cpu_frame);
        return;
    }

    std::vector<uint8_t> rgb(width * height * 3);
    uint8_t* dst_data[1] = { rgb.data() };
    int dst_stride[1] = { width * 3 };
    sws_scale(sws, cpu_frame->data, cpu_frame->linesize, 0, height, dst_data, dst_stride);
    sws_freeContext(sws);
    av_frame_free(&cpu_frame);

    saveJpeg(path, rgb.data(), width, height);

    LOG_DEBUG("Exported decoded frame: {}", path);
}

} // namespace nvr

#endif // DUMP_DECTECT_IMAGE
