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

std::vector<fs::path> VideoSegmentExtractor::processRawVideo(const fs::path& raw_video_path, const fs::path& csv_log_path) {
    LOG_INFO("Processing raw video: {}", raw_video_path.string());

    std::vector<fs::path> extracted_files;  // 收集本次切出的最终路径

    // 1. 解析流 ID 和店铺 ID
    m_stream_id = extractStreamId(raw_video_path);
    LOG_INFO("Extracted stream ID: {}", m_stream_id);

    // 2. 打开视频文件获取时间基准（先开视频：解析 CSV 时需要 m_video_time_base 把 ss 换算回 PTS）
    AVFormatContext* input_ctx = nullptr;
    int ret = avformat_open_input(&input_ctx, raw_video_path.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open video file {}: {}", raw_video_path.string(), ret);
        return extracted_files;
    }

    // 获取视频流信息
    ret = avformat_find_stream_info(input_ctx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to get stream info: {}", ret);
        avformat_close_input(&input_ctx);
        return extracted_files;
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
        return extracted_files;
    }

    LOG_INFO("Video stream index: {}, time base: {}/{}", video_stream_index, m_video_time_base.num, m_video_time_base.den);

    // 3. 解析 CSV 检测日志（ss → PTS）
    auto log_entries = parseDetectionLog(csv_log_path);
    if (log_entries.empty()) {
        LOG_WARN("No detection log entries found in {}", csv_log_path.string());
        avformat_close_input(&input_ctx);
        return extracted_files;
    }
    LOG_INFO("Parsed {} detection log entries", log_entries.size());

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
        return extracted_files;
    }

    // 7. 提取每个片段到 filter 目录
    fs::path filter_dir = fs::path(m_config.record.output_dir) / m_config.record.filter_subdir;
    fs::create_directories(filter_dir);

    // 解析原始 raw 分片的开始时间（文件名中的 start_datetime），作为 filter 时间锚点
    auto raw_start_time = extractRawStartTime(raw_video_path);
    double time_base_d = av_q2d(m_video_time_base);

    int extracted_count = 0;
    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];

        // 先提取到临时文件，获取实际时间后再重命名（必须 .mp4 扩展名，FFmpeg 依赖扩展名判断格式）
        std::string temp_filename = "." + m_stream_id + "_seg_" + std::to_string(i) + ".mp4";
        fs::path temp_path = filter_dir / temp_filename;

        LOG_INFO("Extracting segment {}/{}: {:.1f}s", i + 1, segments.size(), segment.duration_seconds);

        ExtractTimingInfo timing_info;
        if (extractSegment(raw_video_path, segment, temp_path, i, timing_info)) {
            // filter 开始时间 = raw 开始时间 + 首帧相对 raw 的相对时间（first_frame_pts × time_base）
            double first_ss = static_cast<double>(timing_info.first_frame_pts) * time_base_d;
            auto actual_start = raw_start_time + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(first_ss));
            auto actual_end = actual_start + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::duration<double>(timing_info.duration_seconds));

            // 用实际时间生成最终文件名
            PlayerSegment timed_segment = segment;
            timed_segment.start_time = actual_start;
            timed_segment.end_time = actual_end;
            std::string final_filename = generateSegmentFilename(timed_segment, static_cast<int>(i));
            fs::path final_path = filter_dir / final_filename;

            fs::create_directories(final_path.parent_path());
            std::error_code ec;
            fs::rename(temp_path, final_path, ec);
            if (ec) {
                LOG_ERROR("Failed to rename: {} -> {}: {}", temp_path.string(), final_path.string(), ec.message());
            } else {
                extracted_count++;
                extracted_files.push_back(final_path);
                LOG_INFO("Extracted segment: {} ({:.2f}s)", final_path.string(), timing_info.duration_seconds);
            }
        } else {
            LOG_ERROR("Failed to extract segment {}", i);
            std::error_code ec;
            fs::remove(temp_path, ec);
        }
    }

    LOG_INFO("Extraction complete: {}/{} segments extracted", extracted_count, segments.size());

    avformat_close_input(&input_ctx);
    return extracted_files;
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

        if (fields.size() < 5) {
            LOG_WARN("Invalid CSV line: {}", line);
            continue;
        }

        DetectionLogEntry entry;
        try {
            // 解析 ss（HH:MM:SS.mmm，相对原始分片开始的秒数）并换算为 PTS
            int hh = 0, mm = 0, ssv = 0, msv = 0;
            char c1 = 0, c2 = 0, c3 = 0;
            std::istringstream ss_stream(fields[0]);
            if (!(ss_stream >> hh >> c1 >> mm >> c2 >> ssv >> c3 >> msv) ||
                c1 != ':' || c2 != ':' || c3 != '.') {
                LOG_WARN("Invalid ss format: {}", fields[0]);
                continue;
            }
            double seconds = hh * 3600 + mm * 60 + ssv + msv / 1000.0;
            double tb = av_q2d(m_video_time_base);
            entry.frame_pts = (tb > 0) ? static_cast<int64_t>(seconds / tb + 0.5) : 0;

            entry.has_player = (fields[1] == "true");
            entry.player_count = std::stoi(fields[2]);
            entry.npc_count = std::stoi(fields[3]);
            // fields[4] 是 boxes_json，暂时不需要解析

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
                                          int segment_index,
                                          ExtractTimingInfo& timing_info) {
    timing_info = {};
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
    int64_t video_pts_offset = 0;    // 视频 PTS 偏移量
    int64_t audio_pts_offset = 0;    // 音频 PTS 偏移量
    bool audio_offset_set = false;
    int64_t last_video_pts_raw = 0;  // 最后一帧视频包的原始 PTS

    bool has_written_keyframe = false;

    while (av_read_frame(input_ctx, packet) >= 0) {
        // 只处理视频和音频流
        if (packet->stream_index == video_stream_index ||
            (audio_stream_index >= 0 && packet->stream_index == audio_stream_index)) {

            // 检查是否超过结束时间（基于视频 PTS）
            if (packet->stream_index == video_stream_index && packet->pts > end_pts) {
                av_packet_unref(packet);
                break;
            }

            // 在找到第一个视频关键帧之前，跳过所有包（包括音频）
            if (!has_written_keyframe) {
                if (packet->stream_index == video_stream_index &&
                    (packet->flags & AV_PKT_FLAG_KEY)) {
                    has_written_keyframe = true;
                    video_pts_offset = packet->pts;
                    timing_info.first_frame_pts = packet->pts;
                } else {
                    av_packet_unref(packet);
                    continue;
                }
            }

            // 追踪最后一帧视频 PTS（PTS 调整之前，仍在原始空间）
            if (packet->stream_index == video_stream_index) {
                last_video_pts_raw = packet->pts;
            }

            // 设置音频 PTS 偏移量（以第一个到达的音频包为准）
            if (packet->stream_index == audio_stream_index && !audio_offset_set) {
                audio_pts_offset = packet->pts;
                audio_offset_set = true;
            }

            // 调整 PTS/DTS（音视频使用各自的偏移量）
            int64_t offset = (packet->stream_index == video_stream_index) ? video_pts_offset : audio_pts_offset;
            packet->pts -= offset;
            packet->dts -= offset;

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

    // 计算实际视频时长
    timing_info.last_frame_pts = last_video_pts_raw;
    if (last_video_pts_raw > timing_info.first_frame_pts) {
        int64_t pts_diff = last_video_pts_raw - timing_info.first_frame_pts;
        timing_info.duration_seconds = pts_diff * av_q2d(m_video_time_base);
    }

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

std::string VideoSegmentExtractor::lookupStreamName() const {
    for (const auto& stream : m_config.streams) {
        if (stream.id == m_stream_id) {
            return stream.name;
        }
    }
    return m_stream_id;
}

std::string VideoSegmentExtractor::generateSegmentFilename(const PlayerSegment& segment,
                                                          int segment_index) {
    std::string result = m_config.record.filename_template;

    auto formatTime = [](std::time_t t, const char* fmt) -> std::string {
        std::tm tm = *std::localtime(&t);
        std::ostringstream ss;
        ss << std::put_time(&tm, fmt);
        return ss.str();
    };

    std::time_t start_t = std::chrono::system_clock::to_time_t(segment.start_time);
    std::time_t end_t = std::chrono::system_clock::to_time_t(segment.end_time);
    int64_t dur_secs = static_cast<int64_t>(segment.duration_seconds);

    std::map<std::string, std::string> vars;
    vars["{stream_id}"] = m_stream_id;
    vars["{stream_name}"] = lookupStreamName();
    vars["{shop_id}"] = std::to_string(m_shop_id);
    vars["{start_datetime}"] = formatTime(start_t, "%Y%m%d_%H%M%S");
    vars["{start_date}"] = formatTime(start_t, "%Y-%m-%d");
    vars["{start_time}"] = formatTime(start_t, "%H%M%S");
    vars["{end_datetime}"] = formatTime(end_t, "%Y%m%d_%H%M%S");
    vars["{end_time}"] = formatTime(end_t, "%H%M%S");
    vars["{duration_seconds}"] = std::to_string(dur_secs);

    // duration → HHMMSS
    int hours = static_cast<int>(dur_secs / 3600);
    int mins = static_cast<int>((dur_secs % 3600) / 60);
    int secs = static_cast<int>(dur_secs % 60);
    std::ostringstream dur_ss;
    dur_ss << std::setfill('0') << std::setw(2) << hours
           << std::setw(2) << mins << std::setw(2) << secs;
    vars["{duration}"] = dur_ss.str();

    vars["{segment_index}"] = std::to_string(segment_index);

    for (const auto& var : vars) {
        size_t pos = 0;
        while ((pos = result.find(var.first, pos)) != std::string::npos) {
            result.replace(pos, var.first.length(), var.second);
            pos += var.second.length();
        }
    }

    // 如果模板已包含目录路径（如 {stream_id}/{start_date}/...），直接使用
    // 否则追加默认目录结构 {stream_id}/{yyyy-MM-dd}/
    if (!fs::path(result).has_parent_path()) {
        std::string date_str = formatTime(start_t, "%Y-%m-%d");
        result = (fs::path(m_stream_id) / date_str / result).string();
    }

    return result;
}

std::chrono::system_clock::time_point VideoSegmentExtractor::extractRawStartTime(const fs::path& raw_video_path) const {
    // 从 raw 文件名中提取 start_datetime（YYYYMMDD_HHMMSS）作为原始分片开始时间
    std::string filename = raw_video_path.filename().string();
    std::regex re(R"((\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2}))");
    std::smatch match;
    if (std::regex_search(filename, match, re) && match.size() >= 7) {
        try {
            std::tm tm = {};
            tm.tm_year = std::stoi(match[1].str()) - 1900;
            tm.tm_mon = std::stoi(match[2].str()) - 1;
            tm.tm_mday = std::stoi(match[3].str());
            tm.tm_hour = std::stoi(match[4].str());
            tm.tm_min = std::stoi(match[5].str());
            tm.tm_sec = std::stoi(match[6].str());
            tm.tm_isdst = -1;
            return std::chrono::system_clock::from_time_t(std::mktime(&tm));
        } catch (const std::exception& e) {
            LOG_WARN("Failed to parse raw start time from {}: {}", filename, e.what());
        }
    } else {
        LOG_WARN("No start_datetime pattern in filename: {}", filename);
    }
    // 回退：使用当前时间
    return std::chrono::system_clock::now();
}

std::string VideoSegmentExtractor::extractStreamId(const fs::path& video_path) {
    // 优先从目录结构提取：raw/{stream_id}/.../file.mp4
    fs::path p = video_path.parent_path();
    while (!p.empty() && p.parent_path().filename() != m_config.record.raw_subdir) {
        p = p.parent_path();
    }
    if (!p.empty() && p.parent_path().filename() == m_config.record.raw_subdir) {
        std::string candidate = p.filename().string();
        for (const auto& stream : m_config.streams) {
            if (stream.id == candidate) {
                return candidate;
            }
        }
    }

    // 后备：从文件名正则提取
    std::string filename = video_path.stem().string();
    std::regex regex(R"((\d+)_(.+?)_\d{8}_\d{6})");
    std::smatch match;
    if (std::regex_search(filename, match, regex)) {
        return match[2].str();
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
