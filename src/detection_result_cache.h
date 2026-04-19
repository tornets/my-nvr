//
// Created by Claude on 2026/4/19.
// 检测结果缓存 - 用于智能录制的滑动窗口检测
//

#ifndef NVR_DETECTION_RESULT_CACHE_H
#define NVR_DETECTION_RESULT_CACHE_H

#include "detection_types.h"
#include "config_loader.h"
#include <deque>
#include <mutex>
#include <chrono>
#include <cstdint>

namespace nvr::detection {

// 缓存条目
struct DetectionCacheEntry {
    DetectionResult result;
    int64_t frame_pts;
    std::chrono::system_clock::time_point timestamp;

    DetectionCacheEntry()
        : frame_pts(0)
        , timestamp(std::chrono::system_clock::now()) {}
};

// 检测结果缓存（滑动窗口）
class DetectionResultCache {
public:
    explicit DetectionResultCache(int max_duration_seconds = 60);
    ~DetectionResultCache() = default;

    // 禁止拷贝
    DetectionResultCache(const DetectionResultCache&) = delete;
    DetectionResultCache& operator=(const DetectionResultCache&) = delete;

    // 添加检测结果
    void addResult(const DetectionResult& result, int64_t frame_pts);

    // 检查指定时间窗口内是否有玩家
    bool hasPlayerInWindow(int window_seconds) const;

    // 检查指定时间窗口内是否有 NPC
    bool hasNPCInWindow(int window_seconds) const;

    // 获取玩家存在比例（0.0 - 1.0）
    float getPlayerPresenceRatio(int window_seconds) const;

    // 获取 NPC 存在比例（0.0 - 1.0）
    float getNPCPresenceRatio(int window_seconds) const;

    // 获取玩家最大置信度
    float getPlayerMaxConfidence(int window_seconds) const;

    // 获取 NPC 最大置信度
    float getNPCMaxConfidence(int window_seconds) const;

    // 获取指定时间窗口内的平均处理时间
    double getAvgProcessingTime(int window_seconds) const;

    // 清理旧条目
    void cleanOldEntries(int keep_seconds);

    // 清空缓存
    void clear();

    // 获取缓存条目数量
    size_t size() const;

    // 检查缓存是否为空
    bool empty() const;

    // 获取最早的 PTS
    int64_t getFirstPTS() const;

    // 获取最晚的 PTS
    int64_t getLastPTS() const;

    // 获取缓存时长（秒）
    double getDurationSeconds() const;

private:
    mutable std::mutex mutex_;
    std::deque<DetectionCacheEntry> cache_;
    int max_duration_seconds_;
};

// 智能分段决策器
class SmartSegmentDecision {
public:
    explicit SmartSegmentDecision(const SmartRecordingConfig& config);
    ~SmartSegmentDecision() = default;

    // 禁止拷贝
    SmartSegmentDecision(const SmartSegmentDecision&) = delete;
    SmartSegmentDecision& operator=(const SmartSegmentDecision&) = delete;

    // 评估是否应该保存当前分段
    bool shouldSaveSegment(const DetectionResultCache& cache, int64_t segment_start_pts) const;

    // 评估是否应该继续录制
    bool shouldContinueRecording(const DetectionResult& last_result) const;

    // 获取决策原因（用于日志）
    std::string getDecisionReason() const { return decision_reason_; }

private:
    SmartRecordingConfig config_;
    mutable std::string decision_reason_;

    // 生成决策原因
    void setReason(const std::string& reason) const {
        decision_reason_ = reason;
    }
};

} // namespace nvr::detection

#endif // NVR_DETECTION_RESULT_CACHE_H
