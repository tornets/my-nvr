//
// Created by Claude on 2026/4/19.
// 智能录制管理器实现
//

#include "smart_recording_manager.h"
#include "config_loader.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace nvr {

// ============================================================================
// SmartRecordingManager 实现
// ============================================================================

SmartRecordingManager::SmartRecordingManager(
    const SmartRecordingConfig& config,
    const std::string& stream_id)
    : config_(config)
    , stream_id_(stream_id)
    , initialized_(false)
    , running_(false)
    , current_state_(SmartRecordingState::IDLE)
    , segment_start_pts_(0)
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
        logger_->warn("SmartRecordingManager already initialized for stream: {}", stream_id_);
        return true;
    }

    logger_->info("Initializing SmartRecordingManager for stream: {}", stream_id_);

    // 检查配置
    if (!config_.enabled) {
        logger_->info("Smart recording disabled for stream: {}", stream_id_);
        return true;
    }

    if (!config_.rknn.enabled) {
        logger_->warn("RKNN detection disabled, smart recording will not work properly");
        return false;
    }

    // 创建 RKNN 检测器
    detection::DetectionConfig det_config;
    det_config.model_path = config_.rknn.model_path;
    det_config.player_class_id = config_.rknn.player_class_id;
    det_config.npc_class_id = config_.rknn.npc_class_id;
    det_config.confidence_threshold = config_.rknn.confidence_threshold;
    det_config.detection_interval_keyframes = config_.rknn.detection_interval_keyframes;
    det_config.zero_copy_enabled = config_.rknn.zero_copy_enabled;
    rknn_detector_ = std::make_unique<detection::RKNNDetector>(det_config);
    if (!rknn_detector_->initialize()) {
        logger_->error("Failed to initialize RKNN detector for stream: {}", stream_id_);
        return false;
    }

    // 预热模型
    if (!rknn_detector_->warmup()) {
        logger_->warn("RKNN detector warmup failed for stream: {}", stream_id_);
    }

    // 创建帧缓冲区
    frame_buffer_ = std::make_unique<FrameBuffer>(config_.prebuffer_duration_seconds);

    // 创建检测结果缓存
    detection_cache_ = std::make_unique<detection::DetectionResultCache>(
        config_.segment_duration_seconds);

    // 创建分段决策器
    segment_decision_ = std::make_unique<detection::SmartSegmentDecision>(config_);

    initialized_ = true;
    logger_->info("SmartRecordingManager initialized successfully for stream: {}", stream_id_);

    return true;
}

void SmartRecordingManager::shutdown() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) {
            return;
        }
        logger_->info("Shutting down SmartRecordingManager for stream: {}", stream_id_);
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
    rknn_detector_.reset();
    frame_buffer_.reset();
    detection_cache_.reset();
    segment_decision_.reset();

    initialized_ = false;
    logger_->info("SmartRecordingManager shut down for stream: {}", stream_id_);
}

bool SmartRecordingManager::start() {
    std::lock_guard<std::mutex> lock(state_mutex_);

    if (!initialized_) {
        logger_->error("Cannot start: SmartRecordingManager not initialized");
        return false;
    }

    if (running_) {
        logger_->warn("SmartRecordingManager already running for stream: {}", stream_id_);
        return true;
    }

    logger_->info("Starting SmartRecordingManager for stream: {}", stream_id_);

    running_ = true;
    current_state_ = SmartRecordingState::IDLE;
    keyframe_count_ = 0;
    detection_count_ = 0;
    player_detected_count_ = 0;

    // 启动检测线程
    detection_thread_ = std::thread(&SmartRecordingManager::detectionWorkerThread, this);

    logger_->info("SmartRecordingManager started successfully for stream: {}", stream_id_);

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

    logger_->info("Stopping SmartRecordingManager for stream: {}", stream_id_);

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

    logger_->info("SmartRecordingManager stopped for stream: {}", stream_id_);
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
    if (shouldRunDetection(is_key_frame) && decoded_frame) {
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
    logger_->debug("Detection worker thread started for stream: {}", stream_id_);

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
        if (!task.frame || !rknn_detector_) {
            continue;
        }

        detection::DetectionResult result;
        bool success = false;

        if (task.frame->format == AV_PIX_FMT_DRM_PRIME) {
            // DRM_PRIME 帧：只能用零拷贝路径
            auto wrapper = detection::DMABufferExtractor::extractFromAVFrame(task.frame);
            if (wrapper && wrapper->isValid()) {
                success = rknn_detector_->detectFrameZeroCopy(wrapper->getInfo(), result);
            }
            if (!success) {
                logger_->warn("Zero-copy detection failed for DRM_PRIME frame");
            }
        } else {
            // 非 DRM_PRIME 帧：CPU 路径
            success = rknn_detector_->detectFrame(task.frame, result);
        }

        // 释放帧
        if (task.frame) {
            av_frame_free(&task.frame);
        }  // clone 的帧需要 av_frame_free

        if (success) {
            detection_count_++;
            logger_->debug("Detection #{}: {} boxes, has_player={}, player_conf={:.2f}, time={:.1f}ms",
                           detection_count_.load(), result.boxes.size(),
                           result.has_player, result.player_confidence,
                           result.processing_time_ms);
            handleDetectionResult(result);
        } else {
            logger_->warn("Detection failed for frame pts={}", task.pts);
        }
    }

    logger_->debug("Detection worker thread stopped for stream: {}", stream_id_);
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
                logger_->info("Player detected, starting recording (stream: {})", stream_id_);
                should_write_ = true;
                transitionTo(SmartRecordingState::RECORDING);
            }
            break;

        case SmartRecordingState::RECORDING:
            if (!result.has_player) {
                // 玩家消失，进入延迟停止状态
                int delay = config_.post_recording_delay_seconds;
                recording_stop_time_ = std::chrono::steady_clock::now() + std::chrono::seconds(delay);
                logger_->info("Player disappeared, post-recording delay {}s (stream: {})", delay, stream_id_);
                transitionTo(SmartRecordingState::POST_RECORDING);
            }
            break;

        case SmartRecordingState::POST_RECORDING:
            if (result.has_player) {
                // 玩家重新出现，继续录制
                logger_->info("Player reappeared, resuming recording (stream: {})", stream_id_);
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

    logger_->info("State transition: {} -> {} (stream: {})",
                 getStateName(current_state_), getStateName(new_state), stream_id_);

    // 进入 IDLE 状态时停止写入
    if (new_state == SmartRecordingState::IDLE) {
        should_write_ = false;
    }

    current_state_ = new_state;
}

bool SmartRecordingManager::shouldRunDetection(bool is_key_frame) {
    if (!is_key_frame) {
        return false;  // 只检测关键帧
    }

    if (!config_.rknn.enabled) {
        return false;
    }

    // 检查检测间隔
    int interval = config_.rknn.detection_interval_keyframes;
    if (interval <= 0) {
        return false;
    }

    return (keyframe_count_ % interval) == 0;
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

    // 找到最近的关键帧作为起始点
    size_t start_idx = 0;
    for (size_t i = 0; i < all_frames.size(); i++) {
        if (all_frames[i].is_key_frame) {
            start_idx = i;
        }
    }

    // 从最近的关键帧开始返回
    if (start_idx > 0) {
        std::vector<CachedFrame> result;
        for (size_t i = start_idx; i < all_frames.size(); i++) {
            result.push_back(std::move(all_frames[i]));
        }
        return result;
    }

    return all_frames;
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
    if (rknn_detector_) {
        return rknn_detector_->getStats();
    }
    return DetectionStats();
}

void SmartRecordingManager::resetDetectionStats() {
    if (rknn_detector_) {
        rknn_detector_->resetStats();
    }
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
        logger_->error("Output context not set, cannot write cached frames");
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
