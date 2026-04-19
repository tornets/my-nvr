//
// Created by Claude on 2026/4/19.
// 检测结果缓存实现
//

#include "detection_result_cache.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <numeric>

namespace nvr::detection {

// ============================================================================
// DetectionResultCache 实现
// ============================================================================

DetectionResultCache::DetectionResultCache(int max_duration_seconds)
    : max_duration_seconds_(max_duration_seconds)
{
    cache_.clear();
}

void DetectionResultCache::addResult(const DetectionResult& result, int64_t frame_pts) {
    std::lock_guard<std::mutex> lock(mutex_);

    DetectionCacheEntry entry;
    entry.result = result;
    entry.frame_pts = frame_pts;
    entry.timestamp = std::chrono::system_clock::now();

    cache_.push_back(entry);

    // 自动清理旧条目
    cleanOldEntries(max_duration_seconds_);
}

bool DetectionResultCache::hasPlayerInWindow(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return false;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    // 检查是否有玩家检测结果
    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time && entry.result.has_player) {
            return true;
        }
    }

    return false;
}

bool DetectionResultCache::hasNPCInWindow(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return false;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    // 检查是否有 NPC 检测结果
    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time && entry.result.has_npc) {
            return true;
        }
    }

    return false;
}

float DetectionResultCache::getPlayerPresenceRatio(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return 0.0f;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    int total_count = 0;
    int player_count = 0;

    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time) {
            total_count++;
            if (entry.result.has_player) {
                player_count++;
            }
        }
    }

    return total_count > 0 ? static_cast<float>(player_count) / total_count : 0.0f;
}

float DetectionResultCache::getNPCPresenceRatio(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return 0.0f;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    int total_count = 0;
    int npc_count = 0;

    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time) {
            total_count++;
            if (entry.result.has_npc) {
                npc_count++;
            }
        }
    }

    return total_count > 0 ? static_cast<float>(npc_count) / total_count : 0.0f;
}

float DetectionResultCache::getPlayerMaxConfidence(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return 0.0f;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    float max_confidence = 0.0f;
    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time && entry.result.has_player) {
            if (entry.result.player_confidence > max_confidence) {
                max_confidence = entry.result.player_confidence;
            }
        }
    }

    return max_confidence;
}

float DetectionResultCache::getNPCMaxConfidence(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return 0.0f;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    float max_confidence = 0.0f;
    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time && entry.result.has_npc) {
            if (entry.result.npc_confidence > max_confidence) {
                max_confidence = entry.result.npc_confidence;
            }
        }
    }

    return max_confidence;
}

double DetectionResultCache::getAvgProcessingTime(int window_seconds) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.empty()) {
        return 0.0;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(window_seconds);

    double total_time = 0.0;
    int count = 0;

    for (const auto& entry : cache_) {
        if (entry.timestamp >= cutoff_time) {
            total_time += entry.result.processing_time_ms;
            count++;
        }
    }

    return count > 0 ? total_time / count : 0.0;
}

void DetectionResultCache::cleanOldEntries(int keep_seconds) {
    if (cache_.size() < 2) {
        return;
    }

    // 计算截止时间
    auto cutoff_time = std::chrono::system_clock::now() - std::chrono::seconds(keep_seconds);

    // 查找第一个应该保留的条目
    auto erase_end = cache_.begin();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->timestamp >= cutoff_time) {
            erase_end = it;
            break;
        }
    }

    // 删除旧条目
    if (erase_end != cache_.begin()) {
        size_t erase_count = std::distance(cache_.begin(), erase_end);
        spdlog::trace("Cleaning up {} old detection entries from cache", erase_count);
        cache_.erase(cache_.begin(), erase_end);
    }
}

void DetectionResultCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

size_t DetectionResultCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

bool DetectionResultCache::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.empty();
}

int64_t DetectionResultCache::getFirstPTS() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.empty() ? 0 : cache_.front().frame_pts;
}

int64_t DetectionResultCache::getLastPTS() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.empty() ? 0 : cache_.back().frame_pts;
}

double DetectionResultCache::getDurationSeconds() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cache_.size() < 2) {
        return 0.0;
    }

    auto first_time = cache_.front().timestamp;
    auto last_time = cache_.back().timestamp;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(last_time - first_time);

    return static_cast<double>(duration.count()) / 1000.0;
}

// ============================================================================
// SmartSegmentDecision 实现
// ============================================================================

SmartSegmentDecision::SmartSegmentDecision(const SmartRecordingConfig& config)
    : config_(config)
    , decision_reason_()
{
}

bool SmartSegmentDecision::shouldSaveSegment(
    const DetectionResultCache& cache,
    int64_t segment_start_pts) const {

    // 获取分段时长范围内的检测结果
    int segment_duration = config_.segment_duration_seconds;

    // 检查是否有玩家
    bool has_player = cache.hasPlayerInWindow(segment_duration);
    if (!has_player) {
        setReason("No player detected in segment, discarding");
        return false;
    }

    // 计算玩家存在比例
    float player_ratio = cache.getPlayerPresenceRatio(segment_duration);
    if (player_ratio < 0.1f) {  // 至少 10% 的时间有玩家
        setReason("Player presence ratio too low: {:.2f}%, discarding");
        return false;
    }

    setReason("Player detected in segment, saving");
    return true;
}

bool SmartSegmentDecision::shouldContinueRecording(const DetectionResult& last_result) const {
    // 如果检测到玩家，继续录制
    if (last_result.has_player) {
        setReason("Player detected, continuing recording");
        return true;
    }

    setReason("No player detected, may stop recording");
    return false;
}

} // namespace nvr::detection
