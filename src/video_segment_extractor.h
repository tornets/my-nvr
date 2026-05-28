//
// Created by Claude on 2026/5/28.
// 视频片段提取器 - 从 raw 视频中提取有人片段
//

#ifndef NVR_VIDEO_SEGMENT_EXTRACTOR_H
#define NVR_VIDEO_SEGMENT_EXTRACTOR_H

#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include "config_loader.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace fs = std::filesystem;

// 玩家片段信息
struct PlayerSegment {
    int64_t start_pts;                           // 开始 PTS
    int64_t end_pts;                             // 结束 PTS
    std::chrono::system_clock::time_point start_time;  // 开始时间
    int player_count;                            // 玩家数量
    double duration_seconds;                      // 片段时长（秒）
};

// 检测日志记录
struct DetectionLogEntry {
    int64_t frame_pts;
    std::chrono::system_clock::time_point timestamp;
    bool has_player;
    int player_count;
    int npc_count;
};

// 视频片段提取器
class VideoSegmentExtractor {
public:
    explicit VideoSegmentExtractor(const Config& config);
    ~VideoSegmentExtractor();

    // 处理 raw 视频文件，提取有人片段
    // 参数：raw_video_path - raw 视频文件路径，csv_log_path - 对应的 CSV 日志文件路径
    void processRawVideo(const fs::path& raw_video_path, const fs::path& csv_log_path);

private:
    // 读取检测日志，解析玩家片段
    std::vector<PlayerSegment> extractPlayerSegments(
        const std::vector<DetectionLogEntry>& log_entries,
        const AVFormatContext* input_ctx);

    // 从视频中提取指定片段
    bool extractSegment(const fs::path& input_video,
                       const PlayerSegment& segment,
                       const fs::path& output_video,
                       int segment_index);

    // 使用 FFmpeg 定位到最近的 keyframe
    int64_t seekToKeyframe(AVFormatContext* ctx, int64_t target_pts, int stream_index);

    // 生成片段文件名（基于原始文件名，确保与shouldExtractVideo匹配）
    std::string generateSegmentFilename(const PlayerSegment& segment,
                                       const std::string& stream_id,
                                       const std::string& shop_id,
                                       int segment_index,
                                       const std::string& original_filename);

    // 解析 CSV 检测日志
    std::vector<DetectionLogEntry> parseDetectionLog(const fs::path& csv_log_path);

    // 合并相邻片段
    void mergeAdjacentSegments(std::vector<PlayerSegment>& segments);

    // 过滤短片段
    void filterShortSegments(std::vector<PlayerSegment>& segments);

    // 格式化时间戳
    std::string formatTimestamp(const std::chrono::system_clock::time_point& timestamp);
    std::string formatDate(const std::chrono::system_clock::time_point& timestamp);

    // 从文件名解析流 ID 和其他信息
    std::string extractStreamId(const fs::path& video_path);
    int extractShopId(const fs::path& video_path);

    Config m_config;
    AVRational m_video_time_base;  // 视频时间基准
    std::string m_stream_id;       // 流 ID
    int m_shop_id;                 // 店铺 ID
};

#endif // NVR_VIDEO_SEGMENT_EXTRACTOR_H
