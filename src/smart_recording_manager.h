//
// Created by Claude on 2026/4/19.
// 智能录制管理器 - 负责智能录制的决策引擎和状态管理
//

#ifndef NVR_SMART_RECORDING_MANAGER_H
#define NVR_SMART_RECORDING_MANAGER_H

#include "detection_types.h"
#include "detection_result_cache.h"
#include "frame_buffer.h"
#include "detection_pool.h"
#include "config_loader.h"
#include <spdlog/spdlog.h>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>

extern "C" {
#include <libavformat/avformat.h>
}

namespace nvr {

// 引入检测相关类型
using detection::DetectionStats;
using detection::DetectionResult;
using detection::DetectionConfig;
using detection::DetectionResultCache;
using detection::SmartSegmentDecision;
using detection::DetectionPool;
using detection::PoolDetectionResult;

// 录制状态
enum class SmartRecordingState {
    IDLE,           // 空闲状态（预缓存中）
    RECORDING,      // 正在录制
    POST_RECORDING  // 后录制状态（玩家消失后延迟停止）
};

// 检测任务
struct DetectionTask {
    AVFrame* frame;
    int64_t pts;
    int64_t dts;
    bool is_key_frame;

    DetectionTask()
        : frame(nullptr)
        , pts(0)
        , dts(0)
        , is_key_frame(false) {}

    ~DetectionTask() {
        if (frame) {
            // 注意：frame 由调用者管理
        }
    }
};

// 智能录制管理器
class SmartRecordingManager {
public:
    SmartRecordingManager(
        const SmartRecordingConfig& config,
        const std::string& stream_id,
        detection::DetectionPool& detection_pool);
    ~SmartRecordingManager();

    // 禁止拷贝
    SmartRecordingManager(const SmartRecordingManager&) = delete;
    SmartRecordingManager& operator=(const SmartRecordingManager&) = delete;

    // 初始化
    bool initialize();
    void shutdown();

    // 检查是否已初始化
    bool isInitialized() const { return initialized_; }

    // 启动/停止智能录制
    bool start();
    void stop();

    // 检查是否正在运行
    bool isRunning() const { return running_; }

    // 添加视频帧进行处理（decoded_frame 可为 nullptr，非关键帧时不解码）
    bool processVideoFrame(AVPacket* packet, AVFrame* decoded_frame, int64_t pts, int64_t dts, bool is_key_frame);

    // 当前是否应该写入 packet（门控控制）
    bool shouldWritePacket() const;

    // 获取预缓存帧用于写入（检测到玩家开始录制时调用）
    // 返回从最近关键帧开始的所有帧
    std::vector<CachedFrame> getPrebufferFrames() const;

    // 获取状态名称
    static const char* getStateName(SmartRecordingState state);

    // 检查是否应该开始新分段（智能录制模式下始终返回 false）
    bool shouldStartNewSegment() const;

    // 检查是否应该停止录制（带延迟逻辑，会触发状态转换）
    bool shouldStopRecording();

    // 获取缓存的帧（用于写入文件）
    std::vector<CachedFrame> getCachedFrames() const;

    // 获取从指定 PTS 开始的帧
    std::vector<CachedFrame> getFramesSince(int64_t start_pts) const;

    // 清空缓存
    void clearCache();

    // 设置输出上下文（用于写入帧）
    void setOutputContext(AVFormatContext* output_ctx, int video_stream_index);

    // 设置时间基准
    void setTimeBase(AVRational time_base);

    // 获取统计信息
    DetectionStats getDetectionStats() const;
    void resetDetectionStats();

    // 获取配置
    const SmartRecordingConfig& getConfig() const { return config_; }

    // 设置分段开始时间
    void setSegmentStartTime(int64_t pts);

    // 获取分段时长（秒）
    double getSegmentDurationSeconds() const;

#if DUMP_DECTECT_IMAGE
    void setDebugOutputDir(const std::string& dir) { debug_output_dir_ = dir; }
#endif

private:
    // 检测线程工作函数
    void detectionWorkerThread();

    // 处理检测结果
    void handleDetectionResult(const detection::DetectionResult& result);

    // 状态转换
    void transitionTo(SmartRecordingState new_state);

    // 检查是否需要执行检测
    bool shouldRunDetection(bool is_key_frame);

    // 清理旧的检测结果
    void cleanupOldDetections();

    // 写入缓存的帧到输出
    bool writeCachedFrames(const std::vector<CachedFrame>& frames);

    // 日志
    std::shared_ptr<spdlog::logger> logger_;

    // 配置
    SmartRecordingConfig config_;
    std::string stream_id_;

    // 状态
    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
    std::atomic<SmartRecordingState> current_state_;

    // NPU 推理池
    detection::DetectionPool& detection_pool_;

    // 帧缓冲区
    std::unique_ptr<FrameBuffer> frame_buffer_;

    // 检测结果缓存
    std::unique_ptr<detection::DetectionResultCache> detection_cache_;

    // 智能分段决策器
    std::unique_ptr<detection::SmartSegmentDecision> segment_decision_;

    // 门控状态
    std::chrono::steady_clock::time_point player_last_seen_;
    std::chrono::steady_clock::time_point recording_stop_time_;  // 延迟停止的预定时间
    std::atomic<bool> should_write_;

    // 检测队列
    std::queue<DetectionTask> detection_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // 检测线程
    std::thread detection_thread_;

    // 分段时间管理
    int64_t segment_start_pts_;
    AVRational time_base_;

    // 输出上下文
    AVFormatContext* output_ctx_;
    int video_stream_index_;

    // 统计
    std::atomic<int> keyframe_count_;
    std::atomic<int> detection_count_;
    std::atomic<int> player_detected_count_;

    // 线程安全
    mutable std::mutex state_mutex_;
    mutable std::mutex output_mutex_;

#if DUMP_DECTECT_IMAGE
    std::string debug_output_dir_;
    int debug_decode_frame_index_ = 0;
    void saveDetectionImage(const detection::PoolDetectionResult& pool_result, int detect_count);
    void saveDecodedFrame(AVFrame* frame, int frame_idx);
#endif
};

} // namespace nvr

#endif // NVR_SMART_RECORDING_MANAGER_H
