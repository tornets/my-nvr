//
// Created by Claude on 2026/5/28.
// 视频片段提取器实现
//

#include "video_segment_extractor.h"
#include "log.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <map>

VideoSegmentExtractor::VideoSegmentExtractor(const Config& config)
    : m_config(config)
    , m_video_time_base{1, 90000}
    , m_shop_id(config.shop.id)
{
    LOG_INFO("VideoSegmentExtractor initialized");
}

VideoSegmentExtractor::~VideoSegmentExtractor() {
    LOG_DEBUG("VideoSegmentExtractor destroyed");
}

void VideoSegmentExtractor::processRawVideo(const fs::path& raw_video_path, const fs::path& csv_log_path) {
    LOG_INFO("Processing raw video: {}", raw_video_path.string());

    // 1. 解析流 ID 和店铺 ID
    m_stream_id = extractStreamId(raw_video_path);
    LOG_INFO("Extracted stream ID: {}", m_stream_id);

    // 2. 解析 CSV 检测日志
    auto log_entries = parseDetectionLog(csv_log_path);
    if (log_entries.empty()) {
        LOG_WARN("No detection log entries found in {}", csv_log_path.string());
        return;
    }
    LOG_INFO("Parsed {} detection log entries", log_entries.size());

    // 3. 打开视频文件获取时间基准
    AVFormatContext* input_ctx = nullptr;
    int ret = avformat_open_input(&input_ctx, raw_video_path.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open video file {}: {}", raw_video_path.string(), ret);
        return;
    }

    // 获取视频流信息
    ret = avformat_find_stream_info(input_ctx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to get stream info: {}", ret);
        avformat_close_input(&input_ctx);
        return;
    }

    // 找到视频流并获取时间基准
    int video_stream_index = -1;
    for (unsigned i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            m_video_time_base = input_ctx->streams[i]->time_base;
            break;
        }
    }

    if (video_stream_index < 0) {
        LOG_ERROR("No video stream found in {}", raw_video_path.string());
        avformat_close_input(&input_ctx);
        return;
    }

    LOG_INFO("Video stream index: {}, time base: {}/{}", video_stream_index, m_video_time_base.num, m_video_time_base.den);

    // 4. 提取玩家片段
    auto segments = extractPlayerSegments(log_entries, input_ctx);
    LOG_INFO("Extracted {} player segments", segments.size());

    // 5. 合并相邻片段
    mergeAdjacentSegments(segments);
    LOG_INFO("After merging, {} segments remain", segments.size());

    // 6. 过滤短片段
    filterShortSegments(segments);
    LOG_INFO("After filtering, {} segments remain", segments.size());

    if (segments.empty()) {
        LOG_INFO("No valid player segments found, skipping extraction");
        avformat_close_input(&input_ctx);
        return;
    }

    // 7. 提取每个片段到 filter 目录
    fs::path filter_dir = fs::path(m_config.record.output_dir) / m_config.record.filter_subdir;
    fs::create_directories(filter_dir);

    // 获取原始视频文件名（不含扩展名）
    std::string original_filename = raw_video_path.stem().string();

    int extracted_count = 0;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];

        // 生成输出文件名 - 保持原始文件名，确保与shouldExtractVideo的检查匹配
        std::string output_filename = generateSegmentFilename(segment, m_stream_id, std::to_string(m_shop_id), i, original_filename);
        fs::path output_path = filter_dir / output_filename;

        // 创建输出目录
        fs::create_directories(output_path.parent_path());

        LOG_INFO("Extracting segment {}/{}: {}s -> {}", i + 1, segments.size(), segment.duration_seconds, output_path.string());

        // 提取片段
        if (extractSegment(raw_video_path, segment, output_path, i)) {
            extracted_count++;
            LOG_INFO("Successfully extracted segment: {}", output_path.string());
        } else {
            LOG_ERROR("Failed to extract segment: {}", output_path.string());
        }
    }

    LOG_INFO("Extraction complete: {}/{} segments extracted", extracted_count, segments.size());

    avformat_close_input(&input_ctx);
}

std::vector<DetectionLogEntry> VideoSegmentExtractor::parseDetectionLog(const fs::path& csv_log_path) {
    std::vector<DetectionLogEntry> entries;

    std::ifstream file(csv_log_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open CSV log file: {}", csv_log_path.string());
        return entries;
    }

    std::string line;
    bool header_skipped = false;

    while (std::getline(file, line)) {
        if (!header_skipped) {
            header_skipped = true;  // 跳过 CSV 头
            continue;
        }

        if (line.empty()) {
            continue;
        }

        // 解析 CSV 行
        std::istringstream ss(line);
        std::string field;
        std::vector<std::string> fields;

        while (std::getline(ss, field, ',')) {
            // 去除引号
            if (!field.empty() && field[0] == '"') {
                field = field.substr(1, field.size() - 2);
            }
            fields.push_back(field);
        }

        if (fields.size() < 6) {
            LOG_WARN("Invalid CSV line: {}", line);
            continue;
        }

        DetectionLogEntry entry;
        try {
            entry.frame_pts = std::stoll(fields[0]);

            // 解析时间戳
            std::tm tm = {};
            std::istringstream ts_ss(fields[1]);
            ts_ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
            entry.timestamp = std::chrono::system_clock::from_time_t(std::mktime(&tm));

            entry.has_player = (fields[2] == "true");
            entry.player_count = std::stoi(fields[3]);
            entry.npc_count = std::stoi(fields[4]);
            // fields[5] 是 boxes_json，暂时不需要解析

            entries.push_back(entry);
        } catch (const std::exception& e) {
            LOG_WARN("Failed to parse CSV line: {}, error: {}", line, e.what());
        }
    }

    LOG_DEBUG("Parsed {} detection log entries", entries.size());
    return entries;
}

std::vector<PlayerSegment> VideoSegmentExtractor::extractPlayerSegments(
    const std::vector<DetectionLogEntry>& log_entries,
    const AVFormatContext* input_ctx) {

    std::vector<PlayerSegment> segments;
    bool in_player_segment = false;
    PlayerSegment current_segment;

    for (const auto& entry : log_entries) {
        if (entry.has_player) {
            if (!in_player_segment) {
                // 开始新的玩家片段
                in_player_segment = true;
                current_segment.start_pts = entry.frame_pts;
                current_segment.start_time = entry.timestamp;
                current_segment.player_count = entry.player_count;
            }
        } else {
            if (in_player_segment) {
                // 结束当前玩家片段
                in_player_segment = false;
                current_segment.end_pts = entry.frame_pts;

                // 计算时长（秒）
                double pts_diff = current_segment.end_pts - current_segment.start_pts;
                current_segment.duration_seconds = pts_diff * av_q2d(m_video_time_base);

                if (current_segment.duration_seconds > 0) {
                    segments.push_back(current_segment);
                }
            }
        }
    }

    // 处理最后一个片段（如果视频结束时仍然有玩家）
    if (in_player_segment && !log_entries.empty()) {
        current_segment.end_pts = log_entries.back().frame_pts;
        double pts_diff = current_segment.end_pts - current_segment.start_pts;
        current_segment.duration_seconds = pts_diff * av_q2d(m_video_time_base);

        if (current_segment.duration_seconds > 0) {
            segments.push_back(current_segment);
        }
    }

    return segments;
}

void VideoSegmentExtractor::mergeAdjacentSegments(std::vector<PlayerSegment>& segments) {
    if (segments.empty()) {
        return;
    }

    std::vector<PlayerSegment> merged;
    merged.push_back(segments[0]);

    int merge_gap_seconds = m_config.record.player_segment_merge_gap;

    for (size_t i = 1; i < segments.size(); ++i) {
        PlayerSegment& last = merged.back();
        const PlayerSegment& current = segments[i];

        // 计算两个片段之间的间隔（秒）
        double gap_seconds = (current.start_pts - last.end_pts) * av_q2d(m_video_time_base);

        if (gap_seconds <= merge_gap_seconds) {
            // 合并片段
            last.end_pts = current.end_pts;
            last.duration_seconds += current.duration_seconds + gap_seconds;
            last.player_count = std::max(last.player_count, current.player_count);
            LOG_DEBUG("Merged segments: gap {:.2f}s <= {}s", gap_seconds, merge_gap_seconds);
        } else {
            // 不合并，添加新片段
            merged.push_back(current);
        }
    }

    segments = std::move(merged);
}

void VideoSegmentExtractor::filterShortSegments(std::vector<PlayerSegment>& segments) {
    int min_duration = m_config.record.min_player_segment_duration;

    auto it = std::remove_if(segments.begin(), segments.end(),
        [min_duration](const PlayerSegment& seg) {
            return seg.duration_seconds < min_duration;
        });

    size_t removed = std::distance(it, segments.end());
    if (removed > 0) {
        LOG_INFO("Filtered {} segments shorter than {}s", removed, min_duration);
    }

    segments.erase(it, segments.end());
}

bool VideoSegmentExtractor::extractSegment(const fs::path& input_video,
                                          const PlayerSegment& segment,
                                          const fs::path& output_video,
                                          int segment_index) {
    AVFormatContext* input_ctx = nullptr;
    AVFormatContext* output_ctx = nullptr;

    int ret = avformat_open_input(&input_ctx, input_video.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open input video: {}", ret);
        return false;
    }

    ret = avformat_find_stream_info(input_ctx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to find stream info: {}", ret);
        avformat_close_input(&input_ctx);
        return false;
    }

    // 找到视频和音频流
    int video_stream_index = -1;
    int audio_stream_index = -1;

    for (unsigned i = 0; i < input_ctx->nb_streams; i++) {
        if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index < 0) {
            video_stream_index = i;
        } else if (input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index < 0) {
            audio_stream_index = i;
        }
    }

    if (video_stream_index < 0) {
        LOG_ERROR("No video stream found");
        avformat_close_input(&input_ctx);
        return false;
    }

    // 创建输出上下文
    ret = avformat_alloc_output_context2(&output_ctx, nullptr, nullptr, output_video.string().c_str());
    if (ret < 0) {
        LOG_ERROR("Failed to create output context: {}", ret);
        avformat_close_input(&input_ctx);
        return false;
    }

    // 复制流
    for (unsigned i = 0; i < input_ctx->nb_streams; i++) {
        if (i == static_cast<unsigned>(video_stream_index) ||
            (audio_stream_index >= 0 && i == static_cast<unsigned>(audio_stream_index))) {
            AVStream* out_stream = avformat_new_stream(output_ctx, nullptr);
            if (!out_stream) {
                LOG_ERROR("Failed to allocate output stream");
                avformat_close_input(&input_ctx);
                avformat_free_context(output_ctx);
                return false;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, input_ctx->streams[i]->codecpar);
            if (ret < 0) {
                LOG_ERROR("Failed to copy codec parameters: {}", ret);
                avformat_close_input(&input_ctx);
                avformat_free_context(output_ctx);
                return false;
            }

            out_stream->time_base = input_ctx->streams[i]->time_base;
        }
    }

    // 打开输出文件
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&output_ctx->pb, output_video.string().c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("Failed to open output file: {}", ret);
            avformat_close_input(&input_ctx);
            avformat_free_context(output_ctx);
            return false;
        }
    }

    // 写入文件头
    ret = avformat_write_header(output_ctx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to write header: {}", ret);
        avio_closep(&output_ctx->pb);
        avformat_close_input(&input_ctx);
        avformat_free_context(output_ctx);
        return false;
    }

    // Seek 到开始位置（向前到最近的关键帧）
    int64_t start_pts = seekToKeyframe(input_ctx, segment.start_pts, video_stream_index);
    LOG_DEBUG("Seeked to keyframe at PTS {} (target was {})", start_pts, segment.start_pts);

    // 读取并复制数据包
    AVPacket* packet = av_packet_alloc();
    int64_t end_pts = segment.end_pts;
    int64_t pts_offset = 0;  // PTS 偏移量，用于让输出从 0 开始

    bool has_written_keyframe = false;

    while (av_read_frame(input_ctx, packet) >= 0) {
        // 只处理视频和音频流
        if (packet->stream_index == video_stream_index ||
            (audio_stream_index >= 0 && packet->stream_index == audio_stream_index)) {

            // 检查是否超过结束时间
            if (packet->pts > end_pts) {
                av_packet_unref(packet);
                break;
            }

            // 在找到第一个视频关键帧之前，跳过所有包（包括音频）
            // 否则音频包会以原始PTS被写入，导致PTS不连续，视频无法播放
            if (!has_written_keyframe) {
                if (packet->stream_index == video_stream_index &&
                    (packet->flags & AV_PKT_FLAG_KEY)) {
                    has_written_keyframe = true;
                    pts_offset = packet->pts;
                } else {
                    av_packet_unref(packet);
                    continue;
                }
            }

            // 调整 PTS/DTS
            packet->pts -= pts_offset;
            packet->dts -= pts_offset;

            // 确保时间戳为正
            if (packet->pts < 0 || packet->dts < 0) {
                av_packet_unref(packet);
                continue;
            }

            // 写入数据包
            packet->stream_index = (packet->stream_index == static_cast<int>(video_stream_index)) ? 0 :
                                  (audio_stream_index >= 0 ? 1 : -1);

            ret = av_interleaved_write_frame(output_ctx, packet);
            if (ret < 0) {
                LOG_WARN("Failed to write packet: {}", ret);
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    // 写入文件尾
    av_write_trailer(output_ctx);

    // 清理
    if (!(output_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&output_ctx->pb);
    }

    avformat_free_context(output_ctx);
    avformat_close_input(&input_ctx);

    return true;
}

int64_t VideoSegmentExtractor::seekToKeyframe(AVFormatContext* ctx, int64_t target_pts, int stream_index) {
    // 使用 AVSEEK_FLAG_BACKWARD 向前 seek 到最近的关键帧
    int64_t seek_target = target_pts;
    int ret = avformat_seek_file(ctx, stream_index, INT64_MIN, seek_target, INT64_MAX, AVSEEK_FLAG_BACKWARD);

    if (ret < 0) {
        LOG_WARN("Seek failed: {}, using target PTS directly", ret);
        return target_pts;
    }

    // 读取几个包以找到实际的关键帧位置
    AVPacket* packet = av_packet_alloc();
    int64_t actual_pts = target_pts;

    for (int i = 0; i < 100; ++i) {  // 最多检查 100 个包
        if (av_read_frame(ctx, packet) < 0) {
            break;
        }

        if (packet->stream_index == stream_index) {
            if ((packet->flags & AV_PKT_FLAG_KEY) && packet->pts <= target_pts) {
                actual_pts = packet->pts;
            } else if (packet->pts > target_pts) {
                break;
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);

    // 再次 seek 到关键帧位置
    avformat_seek_file(ctx, stream_index, INT64_MIN, actual_pts, INT64_MAX, AVSEEK_FLAG_BACKWARD);

    return actual_pts;
}

std::string VideoSegmentExtractor::generateSegmentFilename(const PlayerSegment& segment,
                                                          const std::string& stream_id,
                                                          const std::string& shop_id,
                                                          int segment_index,
                                                          const std::string& original_filename) {
    // 使用原始文件名，确保与shouldExtractVideo的检查匹配
    // 如果有多个片段，可以添加后缀，但为了简化，这里我们合并所有片段为一个文件

    // 构建文件名（保持原始文件名）
    std::string filename = original_filename + ".mp4";

    // 添加日期目录
    std::string date_str = formatDate(segment.start_time);
    fs::path full_path = fs::path(stream_id) / date_str / filename;

    return full_path.string();
}

std::string VideoSegmentExtractor::formatTimestamp(const std::chrono::system_clock::time_point& timestamp) {
    std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm tm = *std::localtime(&time);

    std::ostringstream ss;
    ss << std::setfill('0');
    ss << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return ss.str();
}

std::string VideoSegmentExtractor::formatDate(const std::chrono::system_clock::time_point& timestamp) {
    std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm tm = *std::localtime(&time);

    std::ostringstream ss;
    ss << std::setfill('0');
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}

std::string VideoSegmentExtractor::extractStreamId(const fs::path& video_path) {
    // 从文件名中提取流 ID
    // 文件名格式：{shop_id}_{stream_id}_{datetime}_{duration}.mp4
    std::string filename = video_path.stem().string();

    // 使用正则表达式提取流 ID
    std::regex regex(R"((\d+)_(.+?)_\d{8}_\d{6})");
    std::smatch match;

    if (std::regex_search(filename, match, regex)) {
        return match[2].str();  // 第二个捕获组是 stream_id
    }

    // 如果正则匹配失败，尝试从父目录获取
    if (video_path.parent_path().filename() != "raw") {
        return video_path.parent_path().filename().string();
    }

    return "unknown";
}

int VideoSegmentExtractor::extractShopId(const fs::path& video_path) {
    // 从文件名中提取店铺 ID
    std::string filename = video_path.stem().string();

    std::regex regex(R"((\d+_.+?)_\d{8}_\d{6})");
    std::smatch match;

    if (std::regex_search(filename, match, regex)) {
        std::string prefix = match[1].str();
        size_t underscore_pos = prefix.find('_');
        if (underscore_pos != std::string::npos) {
            try {
                return std::stoi(prefix.substr(0, underscore_pos));
            } catch (...) {
                // 解析失败，使用默认值
            }
        }
    }

    return m_shop_id;  // 使用配置中的默认值
}
