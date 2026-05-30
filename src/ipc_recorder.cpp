//
// Created by wention on 2026/1/27.
//

#include "log.h"
#include "ipc_recorder.h"
#include "config_loader.h"

#ifdef ENABLE_RKNN_SMART_RECORDING
#include "smart_recording_manager.h"
#include "frame_buffer.h"
#include "detection_logger.h"
#endif

#include <chrono>
#include <iomanip>
#include <sstream>

// av_err2str 在 FFmpeg 新版本中返回临时数组，不能直接传给 fmt::format
static std::string av_err_to_string(int errnum) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), errnum);
    return buf;
}
#include <filesystem>
#include <random>
#include <algorithm>
#include <map>
#include <locale>
#include <codecvt>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
// Windows UTF-8 路径转换辅助函数
std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wstr(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wstr[0], size);
    return wstr;
}

// Windows 特定的创建目录
bool create_directory_utf8(const std::string& path) {
    std::wstring wpath = utf8_to_wide(path);
    return CreateDirectoryW(wpath.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// 递归创建目录
bool create_directories_recursive(const std::string& path) {
    if (path.empty()) return false;

    size_t pos = 0;
    while ((pos = path.find_first_of("\\/", pos + 1)) != std::string::npos) {
        std::string subdir = path.substr(0, pos);
        if (!create_directory_utf8(subdir)) {
            // 检查是否因为已存在而失败
            DWORD attr = GetFileAttributesW(utf8_to_wide(subdir).c_str());
            if (attr == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
    }
    return create_directory_utf8(path);
}

// Windows 特定的文件重命名
bool rename_file_utf8(const std::string& old_path, const std::string& new_path) {
    return MoveFileExW(utf8_to_wide(old_path).c_str(), utf8_to_wide(new_path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
}
#endif

// 静态成员初始化
std::atomic<uint64_t> IPCRecorder::m_global_sequence{1};
std::mutex IPCRecorder::m_global_mutex;

// 中断回调函数（用于超时检测）
int interrupt_callback(void* ctx) {
    IPCRecorder* recorder = static_cast<IPCRecorder*>(ctx);
    if (!recorder) {
        return 0;
    }

    // 如果正在停止，立即中断所有 IO 操作
    if (!recorder->m_running) {
        return 1;
    }

    auto now = std::chrono::steady_clock::now();
    auto last_read = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(
        recorder->m_last_read_time.load()));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_read).count();

    int timeout_ms = recorder->m_timeout_seconds * 1000;
    if (elapsed > timeout_ms) {
        return 1;
    }

    return 0;
}

IPCRecorder::IPCRecorder(const std::string& stream_id, const std::string& stream_url,
                         const std::string& output_dir, const std::string& temp_dir,
                         int segment_duration,
                         const std::string& filename_template,
                         bool enable_audio,
                         bool auto_reconnect,
                         int reconnect_interval_seconds,
                         int max_reconnect_attempts,
                         int timeout_seconds,
                         int shop_id,
                         const std::string& stream_name,
                         int min_segment_duration_seconds
#ifdef ENABLE_RKNN_SMART_RECORDING
                         , const SmartRecordingConfig* smart_recording_config
                         , nvr::detection::DetectionPool* detection_pool
#endif
    )
    : m_stream_id(stream_id)
    , m_stream_name(stream_name.empty() ? stream_id : stream_name)
    , m_stream_url(stream_url)
    , m_output_dir(output_dir)
    , m_temp_dir(temp_dir)
    , m_filename_template(filename_template)
    , m_enable_audio(enable_audio)
    , m_shop_id(shop_id)
    , m_auto_reconnect(auto_reconnect)
    , m_reconnect_interval_seconds(reconnect_interval_seconds)
    , m_max_reconnect_attempts(max_reconnect_attempts)
    , m_timeout_seconds(timeout_seconds)
    , m_min_segment_duration_seconds(min_segment_duration_seconds)
    , m_reconnect_count(0)
    , m_last_packet_time(0)
    , m_last_read_time(0)
    , m_running(false)
    , m_input_ctx(nullptr)
    , m_output_ctx(nullptr)
    , m_video_stream_idx(-1)
    , m_audio_stream_idx(-1)
    , m_audio_decoder_ctx(nullptr)
    , m_audio_encoder_ctx(nullptr)
    , m_audio_fifo(nullptr)
    , m_swr_ctx(nullptr)
    , m_audio_fifo_initialized(0)
    , m_audio_frame_count(0)
    , m_segment_start_pts(0)
    , m_segment_start_dts(0)
    , m_audio_start_pts(0)
    , m_segment_duration(segment_duration)
    , m_segment_index(0)
    , m_segment_start_time(0)
    , m_stream_start_wallclock(0)
    , m_stream_start_pts(0)
    , m_last_video_pts(0)
    , m_last_video_dts(0)
    , m_last_audio_pts(0)
    , m_video_time_base{1, 90000}
    , m_audio_time_base{1, 90000}
    , m_current_dts(0)
    , m_pts_offset(0)
{
#ifdef ENABLE_RKNN_SMART_RECORDING
    m_smart_recording = nullptr;
    m_smart_recording_enabled = false;
    m_last_detection_pts = 0;
    m_video_decoder_ctx = nullptr;
    m_decoded_frame = nullptr;
#endif
    m_logger = spdlog::get("recorder");
    if (!m_logger) {
        m_logger = spdlog::default_logger()->clone("recorder");
    }

    // 解析输出格式（从文件名模板）
    m_output_format = parseOutputFormat();

    // 创建输出目录和临时目录（使用 UTF-8 兼容函数）
#ifdef _WIN32
    create_directories_recursive(m_output_dir);
    create_directories_recursive(m_temp_dir);
#else
    fs::create_directories(m_output_dir);
    fs::create_directories(m_temp_dir);
#endif

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 初始化检测结果日志器（两阶段录制）
    m_detection_logger = std::make_unique<DetectionLogger>(m_output_dir);
    LOG_INFO("Detection logger initialized for stream: {}", m_stream_id);

    // 初始化智能录制
    if (smart_recording_config && smart_recording_config->enabled && detection_pool) {
        m_smart_recording_enabled = true;
        m_smart_recording = std::make_unique<nvr::SmartRecordingManager>(
            *smart_recording_config, m_stream_id, *detection_pool);

        if (m_smart_recording->initialize()) {
            m_smart_recording->start();
            m_smart_recording->setDebugOutputDir(m_output_dir);
            // 设置检测日志器
            m_smart_recording->setDetectionLogger(m_detection_logger.get());
            LOG_INFO("Smart recording enabled for stream: {}", m_stream_id);
        } else {
            LOG_WARN("Failed to initialize smart recording for stream: {}", m_stream_id);
            m_smart_recording_enabled = false;
            m_smart_recording.reset();
        }
    }
#endif

    LOG_INFO("Segment duration: {} seconds", m_segment_duration);
}

IPCRecorder::~IPCRecorder() {
    stop();
}

void IPCRecorder::start() {
    if (m_running) {
        return;
    }
    m_running = true;
    m_thread = std::thread(&IPCRecorder::recordingLoop, this);
}

void IPCRecorder::stop() {
    if (!m_running) {
        return;
    }
    m_running = false;

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 先停止智能录制管理器（释放检测线程）
    if (m_smart_recording) {
        m_smart_recording->shutdown();
    }
#endif

    if (m_thread.joinable()) {
        m_thread.join();
    }

#ifdef ENABLE_RKNN_SMART_RECORDING
    closeHardwareDecoder();
    m_smart_recording.reset();
#endif

    if (m_output_ctx) {
        av_write_trailer(m_output_ctx);
        avio_closep(&m_output_ctx->pb);
        avformat_free_context(m_output_ctx);
        m_output_ctx = nullptr;
    }

    if (m_input_ctx) {
        avformat_close_input(&m_input_ctx);
        m_input_ctx = nullptr;
    }

    if (m_audio_decoder_ctx) {
        avcodec_free_context(&m_audio_decoder_ctx);
        m_audio_decoder_ctx = nullptr;
    }

    if (m_audio_encoder_ctx) {
        avcodec_free_context(&m_audio_encoder_ctx);
        m_audio_encoder_ctx = nullptr;
    }

    if (m_audio_fifo) {
        av_audio_fifo_free(m_audio_fifo);
        m_audio_fifo = nullptr;
    }

    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }
}

void IPCRecorder::recordingLoop() {
    LOG_INFO("Starting recording: {}", m_stream_url);
    LOG_INFO("Auto reconnect: {}, Interval: {}s, Max attempts: {}, Timeout: {}s",
                   m_auto_reconnect, m_reconnect_interval_seconds,
                   m_max_reconnect_attempts == -1 ? "unlimited" : std::to_string(m_max_reconnect_attempts),
                   m_timeout_seconds);

    while (m_running) {
        // 尝试连接并录制
        int prev_reconnect_count = m_reconnect_count.load();
        if (!connectAndRecord()) {
            // 连接或录制失败
            if (!m_auto_reconnect) {
                LOG_WARN("Auto reconnect disabled, stopping recording");
                break;
            }

            // 检查是否达到最大重连次数
            if (m_max_reconnect_attempts != -1 && m_reconnect_count >= m_max_reconnect_attempts) {
                LOG_ERROR("Max reconnect attempts ({}) reached, stopping recording", m_max_reconnect_attempts);
                break;
            }

            // 等待后重连
            LOG_INFO("[{}] Reconnecting in {} seconds...", m_stream_id, m_reconnect_interval_seconds);
            for (int i = 0; i < m_reconnect_interval_seconds && m_running; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        } else if (prev_reconnect_count > 0) {
            LOG_INFO("[{}] Reconnected successfully: {} (after {} attempt(s))", m_stream_id, m_stream_url, prev_reconnect_count);
        }
    }

    LOG_INFO("Recording loop ended: {} (reconnect count: {})", m_stream_url, m_reconnect_count.load());
}

#ifdef ENABLE_RKNN_SMART_RECORDING
bool IPCRecorder::initHardwareDecoder() {
    if (m_video_stream_idx < 0) return false;

    AVStream* video_stream = m_input_ctx->streams[m_video_stream_idx];
    AVCodecID codec_id = video_stream->codecpar->codec_id;

    const char* decoder_name = nullptr;
    if (codec_id == AV_CODEC_ID_H264) {
        decoder_name = "h264_rkmpp";
    } else if (codec_id == AV_CODEC_ID_H265 || codec_id == AV_CODEC_ID_HEVC) {
        decoder_name = "hevc_rkmpp";
    } else {
        LOG_WARN("No rkmpp decoder for codec {}, smart recording disabled", avcodec_get_name(codec_id));
        return false;
    }

    const AVCodec* decoder = avcodec_find_decoder_by_name(decoder_name);
    if (!decoder) {
        LOG_WARN("Decoder {} not found, smart recording disabled", decoder_name);
        return false;
    }

    m_video_decoder_ctx = avcodec_alloc_context3(decoder);
    if (!m_video_decoder_ctx) return false;

    int ret = avcodec_parameters_to_context(m_video_decoder_ctx, video_stream->codecpar);
    if (ret < 0) {
        LOG_ERROR("Failed to copy decoder parameters: {}", av_err_to_string(ret));
        avcodec_free_context(&m_video_decoder_ctx);
        return false;
    }

    // 设置 get_format 回调，选择 DRM_PRIME 输出
    m_video_decoder_ctx->get_format = [](AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) -> AVPixelFormat {
        for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == AV_PIX_FMT_DRM_PRIME) return *p;
        }
        return AV_PIX_FMT_NONE;
    };

    ret = avcodec_open2(m_video_decoder_ctx, decoder, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open hardware decoder: {}", av_err_to_string(ret));
        avcodec_free_context(&m_video_decoder_ctx);
        return false;
    }

    m_decoded_frame = av_frame_alloc();
    if (!m_decoded_frame) {
        avcodec_free_context(&m_video_decoder_ctx);
        return false;
    }

    LOG_INFO("Hardware decoder initialized: {} -> DRM_PRIME", decoder_name);
    return true;
}

void IPCRecorder::closeHardwareDecoder() {
    if (m_decoded_frame) {
        av_frame_free(&m_decoded_frame);
    }
    if (m_video_decoder_ctx) {
        avcodec_free_context(&m_video_decoder_ctx);
    }
}

AVFrame* IPCRecorder::decodeVideoFrame(AVPacket* packet) {
    if (!m_video_decoder_ctx) return nullptr;

    auto t0 = std::chrono::steady_clock::now();

    // 发送 packet 到解码器，EAGAIN 时先排空输出再重试
    int ret = avcodec_send_packet(m_video_decoder_ctx, packet);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            // 解码器输入缓冲区满，排空已解码帧后重试
            while (avcodec_receive_frame(m_video_decoder_ctx, m_decoded_frame) == 0) {
                av_frame_unref(m_decoded_frame);
            }
            ret = avcodec_send_packet(m_video_decoder_ctx, packet);
            if (ret < 0) {
                if (ret != AVERROR(EAGAIN)) {
                    LOG_TRACE("[{}] hw decoder send_packet failed: {}", m_stream_id, av_err_to_string(ret));
                }
                return nullptr;
            }
        } else {
            LOG_TRACE("[{}] hw decoder send_packet failed: {}", m_stream_id, av_err_to_string(ret));
            return nullptr;
        }
    }

    ret = avcodec_receive_frame(m_video_decoder_ctx, m_decoded_frame);
    if (ret < 0) {
        if (ret != AVERROR(EAGAIN)) {
            LOG_TRACE("[{}] hw decoder receive_frame failed: {}", m_stream_id, av_err_to_string(ret));
        }
        return nullptr;
    }

    auto t1 = std::chrono::steady_clock::now();
    auto decode_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    LOG_TRACE("[{}] hw decode ok: pts={}, time={:.1f}ms", m_stream_id, packet->pts, decode_ms);

    if (m_decoded_frame->format != AV_PIX_FMT_DRM_PRIME) {
        LOG_DEBUG("Expected DRM_PRIME, got format {}", m_decoded_frame->format);
        av_frame_unref(m_decoded_frame);
        return nullptr;
    }

    return m_decoded_frame;
}
#endif

bool IPCRecorder::connectAndRecord() {
    // 清理之前的连接
    if (m_input_ctx) {
        avformat_close_input(&m_input_ctx);
        m_input_ctx = nullptr;
    }
    if (m_output_ctx) {
        closeOutput();
    }

    // 重置状态
    m_video_stream_idx = -1;
    m_audio_stream_idx = -1;
    m_segment_index = 0;
    m_pts_offset = 0;
    m_stream_start_wallclock = 0;
    m_stream_start_pts = 0;

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 重置智能录制检测状态（PTS 重连后会从低值重新开始）
    if (m_smart_recording) {
        m_smart_recording->resetForReconnect();
    }
#endif

    // 尝试打开输入
    if (!openInput()) {
        m_last_error = "Failed to open input";
        return false;
    }

    // 设置流
    if (!setupStreams()) {
        m_last_error = "Failed to setup streams";
        return false;
    }

    // 设置音频转码
    if (m_audio_stream_idx != -1 && !setupAudioTranscoding()) {
        LOG_WARN("Failed to setup audio transcoding, will record video only");
        m_audio_stream_idx = -1;
    }

    LOG_INFO("[{}] Connection established, starting recording loop", m_stream_id);

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 初始化硬件解码器（用于检测）
    if (m_smart_recording_enabled) {
        if (initHardwareDecoder()) {
            LOG_INFO("Hardware decoder ready for smart recording");
        } else {
            LOG_WARN("Hardware decoder init failed, detection will be limited");
        }
    }
#endif

    AVPacket* packet = av_packet_alloc();
    auto last_activity_time = std::chrono::steady_clock::now();
    int consecutive_errors = 0;
    const int MAX_CONSECUTIVE_ERRORS = 10;
    const int READ_TIMEOUT_MS = 5000;  // 每次读取的超时时间（毫秒）

    while (m_running) {
        // 检查超时
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_activity_time).count();
        if (elapsed >= m_timeout_seconds) {
            m_last_error = "Stream timeout - no data received";
            LOG_ERROR("[{}] Stream timeout: no data for {} seconds, reconnecting...", m_stream_id, elapsed);
            if (m_output_ctx) {
                closeOutput();
            }
            m_reconnect_count++;
            av_packet_free(&packet);
            return false;
        }

        // 更新最后读取时间（用于中断回调）
        m_last_read_time = now.time_since_epoch().count();

        // 清空包数据以确保干净的状态
        av_packet_unref(packet);

        int ret = av_read_frame(m_input_ctx, packet);
        if (ret < 0) {
            consecutive_errors++;

            // 检查是否是超时中断
            if (ret == AVERROR_EXIT) {
                LOG_WARN("Read interrupted by timeout (error count: {})", consecutive_errors);
            } else if (ret == AVERROR_EOF) {
                LOG_WARN("End of stream (error count: {})", consecutive_errors);
            } else {
                LOG_ERROR("Error reading frame: {} (error count: {})", av_err_to_string(ret), consecutive_errors);
            }

            // 连续错误过多，认为连接已断开
            if (consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
                m_last_error = "Too many consecutive read errors";
                LOG_ERROR("[{}] Too many consecutive errors ({}), reconnecting...", m_stream_id, MAX_CONSECUTIVE_ERRORS);
                if (m_output_ctx) {
                    closeOutput();
                }
                m_reconnect_count++;
                av_packet_free(&packet);
                return false;
            }

            // 短暂等待后继续
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 成功读取帧，重置错误计数
        consecutive_errors = 0;
        last_activity_time = std::chrono::steady_clock::now();
        m_last_packet_time = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        // 验证时间戳有效性
        if (packet->pts < 0 || packet->dts < 0) {
            LOG_DEBUG("Skipping packet with invalid pts/dts: pts={}, dts={}",
                           packet->pts, packet->dts);
            continue;
        }

        // 如果输出文件还未打开，必须等待关键帧
        // 这确保录制从完整画面开始，避免花屏

#ifdef ENABLE_RKNN_SMART_RECORDING
        // 智能录制门控模式
        if (m_smart_recording_enabled && m_smart_recording) {
            // TODO: 两阶段录制重构 - 禁用 POST_RECORDING 停止逻辑
            // 两阶段模式下所有视频连续录制，分段只由 shouldSwitchSegment 控制
            // if (m_output_ctx && m_smart_recording->shouldStopRecording()) {
            //     LOG_INFO("Smart recording: post-recording delay expired, closing segment");
            //     closeOutput();
            //     m_smart_recording->clearCache();
            // }

            // 分段时长检查（智能录制也按配置时长切片）
            if (m_output_ctx && shouldSwitchSegment(packet)) {
                LOG_INFO("Smart recording: segment duration reached, closing segment at key frame pts={}", packet->pts);
                closeOutput();
                m_smart_recording->clearCache();
                // 当前关键帧将作为新分段的首帧，确保分片间无缝衔接
                // 关闭旧分段后继续往下走，写入新分段
            }

            // 不在录制中且门控未开启 → 跳过此包
            // TODO: 两阶段录制重构 - 暂时禁用智能录制门控，所有视频录制到 raw 目录
            // if (!m_output_ctx && !m_smart_recording->shouldWritePacket()) {
            if (false) {
                // IDLE：缓存所有视频帧，按检测模式决定是否解码
                if (packet->stream_index == m_video_stream_idx) {
                    bool is_key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;
                    AVFrame* decoded = nullptr;
                    // 根据检测模式判断是否需要解码
                    bool need_decode = m_smart_recording->shouldDecodeForDetection(is_key_frame, packet->pts);
                    if (m_video_decoder_ctx && need_decode) {
                        decoded = decodeVideoFrame(packet);
                    }
                    m_smart_recording->processVideoFrame(packet, decoded, packet->pts, packet->dts, is_key_frame);
                    if (decoded) {
                        av_frame_unref(m_decoded_frame);
                    }
                }
                av_packet_unref(packet);
                continue;
            }

            // 门控刚开启（IDLE→RECORDING）且未打开输出 → 用预缓存帧立即开始录制
            // TODO: 两阶段录制重构 - 暂时禁用智能录制门控
            // if (!m_output_ctx && m_smart_recording->shouldWritePacket()) {
            if (false) {
                auto prebuffer = m_smart_recording->getPrebufferFrames();

                if (!prebuffer.empty() && prebuffer[0].is_key_frame) {
                    // 预缓存中有完整 GOP（从关键帧开始），立即打开文件并写入
                    LOG_INFO("Smart recording: opening new file with {} prebuffer frames", prebuffer.size());

                    m_segment_index++;
                    std::time(&m_segment_start_time);

                    // 生成临时文件名
                    static const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyz";
                    static std::random_device rd;
                    static std::mt19937 gen(rd());
                    static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);
                    std::string random_str;
                    random_str.reserve(16);
                    for (int i = 0; i < 16; i++) random_str += charset[dis(gen)];

                    std::string ext = ".mp4";
                    if (m_output_format == "matroska") ext = ".mkv";
                    else if (m_output_format == "avi") ext = ".avi";
                    else if (m_output_format == "mov") ext = ".mov";
                    else if (m_output_format == "flv") ext = ".flv";
                    else if (m_output_format == "webm") ext = ".webm";
                    else if (m_output_format == "mpegts") ext = ".ts";

                    std::string filename = "temp_" + random_str + ext;
                    if (!openOutput(filename)) {
                        LOG_ERROR("Failed to open temp file for prebuffer: {}", filename);
                        av_packet_unref(packet);
                        continue;
                    }

                    // 设置智能录制管理器的输出上下文
                    m_smart_recording->setOutputContext(m_output_ctx, 0);
                    m_smart_recording->setTimeBase(m_video_time_base);

                    // 分段起始 PTS 使用预缓存首帧
                    m_segment_start_pts = prebuffer[0].pts;
                    m_segment_start_dts = prebuffer[0].dts >= 0 ? prebuffer[0].dts : prebuffer[0].pts;
                    m_smart_recording->setSegmentStartTime(m_segment_start_pts);

                    // 重置音频状态
                    m_audio_start_pts = 0;
                    m_audio_frame_count = 0;
                    m_last_video_pts = 0;
                    m_last_video_dts = 0;
                    m_last_audio_pts = 0;

                    // 写入预缓存帧（PTS 偏移到从 0 开始）
                    if (!prebuffer.empty() && prebuffer[0].is_key_frame) {
                        // 验证预缓存的完整性
                        bool has_valid_keyframes = true;
                        for (size_t i = 0; i < prebuffer.size(); i++) {
                            if (i == 0 && !prebuffer[i].is_key_frame) {
                                LOG_ERROR("Prebuffer first frame is not a key frame!");
                                has_valid_keyframes = false;
                                break;
                            }
                        }

                        if (!has_valid_keyframes) {
                            LOG_ERROR("Prebuffer validation failed, discarding prebuffer");
                            // 不写入预缓存，等待下一个关键帧
                            if (!(packet->flags & AV_PKT_FLAG_KEY) || packet->stream_index != m_video_stream_idx) {
                                av_packet_unref(packet);
                                continue;
                            }
                        } else {
                            nvr::FrameBufferWriter writer(m_output_ctx, 0);
                            writer.setTimeBase(m_video_time_base);
                            writer.setPTSOffset(m_segment_start_pts, m_segment_start_dts);
                            if (!writer.writeFrames(prebuffer)) {
                                LOG_WARN("Failed to write some prebuffer frames");
                            }

                            // 不更新 m_segment_start_pts 和 m_segment_start_dts
                            // 保持原来的值，这样当前帧的时间戳会相对于预缓存的第一帧，而不是最后一帧
                            // 这确保了 DTS 的连续性

                            LOG_INFO("Smart recording: prebuffer written ({} frames), start pts={} kept for continuity",
                                     prebuffer.size(), m_segment_start_pts);
                            // 当前包继续往下走正常写入流程
                        }
                    } else {
                        // 无预缓存或无关键帧，等待下一个关键帧
                        if (!(packet->flags & AV_PKT_FLAG_KEY) || packet->stream_index != m_video_stream_idx) {
                            av_packet_unref(packet);
                            continue;
                        }
                        LOG_INFO("Smart recording: opening new file (no prebuffer), player detected");
                    }
                }
            }

            // 智能录制模式：等待关键帧检查（两阶段录制重构）
            // 当没有输出上下文时，必须等待关键帧才开始录制
            if (!m_output_ctx) {
                if (packet->stream_index == m_video_stream_idx) {
                    if (!(packet->flags & AV_PKT_FLAG_KEY)) {
                        LOG_DEBUG("Smart recording: waiting for key frame before starting new segment...");
                        av_packet_unref(packet);
                        continue;
                    }
                    LOG_INFO("Smart recording: key frame received, starting new segment");
                }
            }
        } else
#endif
        {
            // 常规模式：等待关键帧
            if (m_output_ctx == nullptr) {
                if (!(packet->flags & AV_PKT_FLAG_KEY)) {
                    LOG_DEBUG("Waiting for key frame before starting recording...");
                    continue;
                }
                LOG_INFO("Key frame received, starting new segment");
            }

            // 常规分段切换
            if (shouldSwitchSegment(packet)) {
                LOG_INFO("Segment duration reached, closing current segment at key frame pts={}", packet->pts);
                closeOutput();
                // 跳过当前关键帧，避免它被写入下一个分段
                // 下一个分段将从下一个关键帧开始
                continue;
            }
        }

        if (packet->stream_index == m_video_stream_idx) {
            if (!m_output_ctx) {
                // 增加分段序号
                m_segment_index++;

                // 记录开始时间
                std::time(&m_segment_start_time);

                // 第一个分段：记录 PTS 锚点，后续分段用此计算文件名时间
                if (m_stream_start_wallclock == 0) {
                    m_stream_start_wallclock = m_segment_start_time;
                    m_stream_start_pts = packet->pts;
                    LOG_INFO("Stream PTS anchor set: wallclock={}, pts={}",
                             m_stream_start_wallclock, m_stream_start_pts);
                }

                // 生成临时文件名（使用随机字符串和目标格式扩展名）
                static const char charset[] = "0123456789abcdefghijklmnopqrstuvwxyz";
                static std::random_device rd;
                static std::mt19937 gen(rd());
                static std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

                std::string random_str;
                random_str.reserve(16);
                for (int i = 0; i < 16; i++) {
                    random_str += charset[dis(gen)];
                }

                // 根据输出格式确定扩展名
                std::string ext = ".mp4";  // 默认
                if (m_output_format == "matroska") {
                    ext = ".mkv";
                } else if (m_output_format == "avi") {
                    ext = ".avi";
                } else if (m_output_format == "mov") {
                    ext = ".mov";
                } else if (m_output_format == "flv") {
                    ext = ".flv";
                } else if (m_output_format == "webm") {
                    ext = ".webm";
                } else if (m_output_format == "mpegts") {
                    ext = ".ts";
                }

                std::string filename = "temp_" + random_str + ext;

                LOG_INFO("Opening temp file: {}", filename);
                if (!openOutput(filename)) {
                    LOG_ERROR("Failed to open temp file: {}", filename);
                    av_packet_unref(packet);
                    continue;
                }
                LOG_INFO("Temp file opened successfully");

                // 验证：确保文件从关键帧开始
                if (!(packet->flags & AV_PKT_FLAG_KEY)) {
                    LOG_ERROR("BUG: First frame written is not a key frame! This should not happen.");
                    closeOutput();
                    av_packet_unref(packet);
                    continue;
                }
                LOG_DEBUG("Validated: Segment starts with key frame at pts={}", packet->pts);

#ifdef ENABLE_RKNN_SMART_RECORDING
                // 设置智能录制管理器的输出上下文
                if (m_smart_recording_enabled && m_smart_recording) {
                    m_smart_recording->setOutputContext(m_output_ctx, 0);  // 视频流在输出的第一个位置
                    m_smart_recording->setTimeBase(m_video_time_base);
                }
#endif

                if (packet->pts > 0) {
                    m_segment_start_pts = packet->pts;
                    // 使用相同帧的 DTS 作为起始 DTS
                    m_segment_start_dts = packet->dts >= 0 ? packet->dts : packet->pts;
                    LOG_DEBUG("Segment start: pts={}, dts={}", m_segment_start_pts, m_segment_start_dts);

#ifdef ENABLE_RKNN_SMART_RECORDING
                    if (m_smart_recording_enabled && m_smart_recording) {
                        m_smart_recording->setSegmentStartTime(m_segment_start_pts);
                    }
#endif
                }

                // 重置音频起始 PTS 和帧计数
                m_audio_start_pts = 0;
                m_audio_frame_count = 0;
                m_last_video_pts = 0;  // 重置最后一个视频PTS
                m_last_video_dts = 0;   // 重置最后一个视频DTS
                m_last_audio_pts = 0;   // 重置最后一个音频PTS
            }

            int stream_index = packet->stream_index;

            if (packet->flags & AV_PKT_FLAG_KEY) {
                LOG_DEBUG("Key frame received at pts={}", packet->pts);
            }

            // 保存最后一个视频包的原始PTS（用于计算实际录制时长）
            if (packet->pts > 0) {
                m_last_video_pts = packet->pts;
            }
            if (packet->dts >= 0) {
                m_last_video_dts = packet->dts;
            }

            // 关键帧统计和验证
            if (packet->flags & AV_PKT_FLAG_KEY) {
                static int keyframe_count = 0;
                keyframe_count++;
                if (keyframe_count % 100 == 0) {  // 每100个关键帧记录一次
                    LOG_DEBUG("Key frame stats: {} keyframes processed, latest pts={}", keyframe_count, packet->pts);
                }
            }

            // 计算相对时间戳并转换到输出时间基准
            AVStream* in_stream = m_input_ctx->streams[stream_index];
            AVStream* out_stream = m_output_ctx->streams[0];  // 视频流在输出的第一个位置

#ifdef ENABLE_RKNN_SMART_RECORDING
            // 保存原始 PTS/DTS 用于智能录制（在调整之前）
            int64_t original_pts = packet->pts;
            int64_t original_dts = packet->dts;
#endif

            // 分别减去起始 PTS 和 DTS
            packet->pts -= m_segment_start_pts;
            packet->dts -= m_segment_start_dts;

            // 确保时间戳非负
            if (packet->pts < 0) packet->pts = 0;
            if (packet->dts < 0) packet->dts = 0;

            // 确保 DTS <= PTS（对于某些编码格式）
            if (packet->dts > packet->pts) {
                LOG_DEBUG("DTS > PTS, setting DTS = PTS: pts={}, dts={}", packet->pts, packet->dts);
                packet->dts = packet->pts;
            }

            // 转换到输出流的时间基准
            av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);
            packet->stream_index = 0;  // 设置为输出视频流索引

#ifdef ENABLE_RKNN_SMART_RECORDING
            // 智能录制处理（使用原始时间戳）
            if (m_smart_recording_enabled && m_smart_recording) {
                bool is_key_frame = (packet->flags & AV_PKT_FLAG_KEY) != 0;

                // 根据检测模式判断是否需要解码
                bool need_decode = m_smart_recording->shouldDecodeForDetection(is_key_frame, original_pts);

                if (need_decode && m_video_decoder_ctx) {
                    AVFrame* decoded = decodeVideoFrame(packet);
                    if (decoded) {
                        m_smart_recording->processVideoFrame(packet, decoded, original_pts, original_dts, is_key_frame);
                        av_frame_unref(m_decoded_frame);
                    } else {
                        m_smart_recording->processVideoFrame(packet, nullptr, original_pts, original_dts, is_key_frame);
                    }
                } else {
                    m_smart_recording->processVideoFrame(packet, nullptr, original_pts, original_dts, is_key_frame);
                }
            }
#endif

            writePacket(packet);
        } else if (packet->stream_index == m_audio_stream_idx && m_output_ctx) {
            // 处理音频包
            if (m_audio_start_pts == 0 && packet->pts > 0) {
                // 记录第一个音频包的 PTS 作为起始点
                m_audio_start_pts = packet->pts;
                LOG_DEBUG("Audio start PTS set to: {}", m_audio_start_pts);
            }

            // 保存最后一个音频包的PTS（用于分段切换判断）
            if (packet->pts > 0) {
                m_last_audio_pts = packet->pts;
            }

            if (m_audio_encoder_ctx) {
                // 需要转码
                transcodeAudio(packet);
            } else {
                // 直接复制音频包
                AVPacket* out_packet = av_packet_clone(packet);
                if (out_packet) {
                    // 找到输出流中的音频流索引
                    int out_audio_idx = -1;
                    for (unsigned i = 0; i < m_output_ctx->nb_streams; i++) {
                        if (m_output_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                            out_audio_idx = i;
                            break;
                        }
                    }

                    if (out_audio_idx >= 0) {
                        out_packet->stream_index = out_audio_idx;
                        // 使用音频自己的起始 PTS 调整时间戳
                        out_packet->pts -= m_audio_start_pts;
                        out_packet->dts -= m_audio_start_pts;
                        av_packet_rescale_ts(out_packet,
                                            m_input_ctx->streams[m_audio_stream_idx]->time_base,
                                            m_output_ctx->streams[out_audio_idx]->time_base);
                        writePacket(out_packet);
                    }
                    av_packet_free(&out_packet);
                }
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    closeOutput();

#ifdef ENABLE_RKNN_SMART_RECORDING
    closeHardwareDecoder();
#endif

    LOG_INFO("Recording stopped: {}", m_stream_url);
    return true;  // 正常结束
}

bool IPCRecorder::openInput() {
    // 分配输入上下文
    m_input_ctx = avformat_alloc_context();
    if (!m_input_ctx) {
        LOG_ERROR("Cannot allocate input context");
        return false;
    }

    // 初始化最后读取时间
    auto now = std::chrono::steady_clock::now();
    m_last_read_time = now.time_since_epoch().count();

    // 设置中断回调
    m_input_ctx->interrupt_callback.callback = interrupt_callback;
    m_input_ctx->interrupt_callback.opaque = this;

    // 设置超时选项（微秒单位）
    // 增加缓冲区以防止帧丢失导致花屏
    m_input_ctx->probesize = 10 * 1024 * 1024;  // 10MB
    m_input_ctx->max_analyze_duration = 10 * AV_TIME_BASE;

    // 注意：不设置 AVFMT_FLAG_NOBUFFER，保留适当缓冲以避免帧丢失
    // 之前使用 NOBUFFER 导致 P 帧/B 帧丢失，造成画面花屏

    // 设置 FFmpeg 选项以增强错误恢复
    AVDictionary* options = nullptr;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用 TCP 传输（更稳定，防止丢包花屏）
    av_dict_set(&options, "stimeout", std::to_string(m_timeout_seconds * 1000000).c_str(), 0);  // RTSP socket 超时（微秒）
    av_dict_set(&options, "fflags", "+genpts+discardcorrupt", 0);  // 生成 PTS 并丢弃损坏的数据包
    av_dict_set(&options, "err_detect", "ignore_err", 0);  // 忽略错误继续解码
    //av_dict_set(&options, "max_delay", "500000", 0);  // 最大延迟 500ms

    LOG_INFO("Opening stream: {} (timeout: {}s, transport: tcp)", m_stream_url, m_timeout_seconds);

    int ret = avformat_open_input(&m_input_ctx, m_stream_url.c_str(), nullptr, &options);

    // 释放选项字典
    if (options) {
        av_dict_free(&options);
    }
    if (ret < 0) {
        if (ret == AVERROR_EXIT) {
            LOG_ERROR("Cannot open input: timeout after {}s", m_timeout_seconds);
        } else {
            LOG_ERROR("Cannot open input: {}", av_err_to_string(ret));
        }
        m_input_ctx = nullptr;
        return false;
    }

    ret = avformat_find_stream_info(m_input_ctx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Cannot find stream info: {}", av_err_to_string(ret));
        avformat_close_input(&m_input_ctx);
        m_input_ctx = nullptr;
        return false;
    }

    // 打印流信息用于调试
    av_dump_format(m_input_ctx, 0, m_stream_url.c_str(), 0);

    return true;
}

bool IPCRecorder::openOutput(const std::string& filename) {
    m_current_filename = filename;
    // 使用临时目录存放正在录制的文件
    std::string full_path = (fs::path(m_temp_dir) / filename).string();

    // TODO: 两阶段录制重构 - 为临时文件创建 CSV 日志（录制后重命名为最终名）
    #ifdef ENABLE_RKNN_SMART_RECORDING
    if (m_detection_logger && m_smart_recording) {
        // 为临时文件创建 CSV 日志文件（在 temp_dir 中）
        fs::path temp_csv_path = fs::path(m_temp_dir) / m_current_filename;
        temp_csv_path.replace_extension(".csv");

        // 创建 CSV 日志文件
        std::string csv_log_path = m_detection_logger->createLogFile(temp_csv_path);
        if (!csv_log_path.empty()) {
            // 设置 SmartRecordingManager 的日志文件路径
            m_smart_recording->setCurrentLogFile(csv_log_path);
            LOG_DEBUG("Set detection log file for current recording: {}", csv_log_path);
        }
    }
    #endif

    // 动态创建输出上下文，使用解析的格式
    int ret = avformat_alloc_output_context2(&m_output_ctx, nullptr, m_output_format.c_str(), full_path.c_str());
    if (ret < 0) {
        LOG_ERROR("Cannot create output context for format {}: {}", m_output_format, av_err_to_string(ret));
        return false;
    }

    LOG_INFO("Creating output file with format: {}", m_output_format);

    // 重置音频编码器（需要重新检查是否需要转码）
    if (m_audio_encoder_ctx) {
        avcodec_free_context(&m_audio_encoder_ctx);
        m_audio_encoder_ctx = nullptr;
    }
    if (m_audio_decoder_ctx) {
        avcodec_free_context(&m_audio_decoder_ctx);
        m_audio_decoder_ctx = nullptr;
    }
    if (m_audio_fifo) {
        av_audio_fifo_free(m_audio_fifo);
        m_audio_fifo = nullptr;
    }
    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
        m_swr_ctx = nullptr;
    }
    m_audio_fifo_initialized = 0;

    // 重置音频转码状态（FIFO 和帧计数器）
    resetAudioTranscodingState();

    bool need_audio_transcode = false;

    for (unsigned i = 0; i < m_input_ctx->nb_streams; i++) {
        AVStream* in_stream = m_input_ctx->streams[i];

        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            // 检查视频编码器是否与输出格式兼容
            AVCodecID video_codec = in_stream->codecpar->codec_id;
            if (!isVideoCodecCompatible(video_codec, m_output_format)) {
                LOG_ERROR("Video codec {} (ID:{}) is not compatible with output format {}. Stream recording aborted.",
                               avcodec_get_name(video_codec), static_cast<int>(video_codec), m_output_format);
                avformat_free_context(m_output_ctx);
                m_output_ctx = nullptr;
                return false;
            }

            LOG_INFO("Video codec {} (ID:{}) is compatible with format {}",
                          avcodec_get_name(video_codec), static_cast<int>(video_codec), m_output_format);

            AVStream* out_stream = avformat_new_stream(m_output_ctx, nullptr);
            if (!out_stream) {
                LOG_ERROR("Failed to allocate output stream");
                return false;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                LOG_ERROR("Failed to copy codec parameters");
                return false;
            }

            m_video_time_base = in_stream->time_base;
            out_stream->time_base = in_stream->time_base;
            out_stream->codecpar->codec_tag = 0;
        } else if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_audio_stream_idx != -1) {
            // 检查音频编码器是否与输出格式兼容
            AVCodecID audio_codec = in_stream->codecpar->codec_id;
            if (!isAudioCodecCompatible(audio_codec, m_output_format)) {
                LOG_WARN("Audio codec {} (ID:{}) is not compatible with output format {}. Will transcode to {}.",
                              avcodec_get_name(audio_codec), static_cast<int>(audio_codec), m_output_format,
                              avcodec_get_name(getBestAudioCodec(m_output_format)));
                need_audio_transcode = true;
            } else {
                LOG_INFO("Audio codec {} (ID:{}) is compatible with format {}",
                              avcodec_get_name(audio_codec), static_cast<int>(audio_codec), m_output_format);
            }

            // 添加音频流到输出
            AVStream* out_stream = avformat_new_stream(m_output_ctx, nullptr);
            if (!out_stream) {
                LOG_ERROR("Failed to allocate audio output stream");
                return false;
            }

            // 先复制输入参数作为占位符（无论是否转码）
            // 这样可以确保 codecpar 有有效的初始值
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                LOG_ERROR("Failed to copy audio codec parameters");
                return false;
            }

            out_stream->time_base = in_stream->time_base;
            out_stream->codecpar->codec_tag = 0;

            LOG_INFO("Added audio stream to output (transcode: {})", need_audio_transcode);
        }
    }

    // 如果需要音频转码，设置转码器
    if (need_audio_transcode) {
        if (!setupAudioTranscoding()) {
            LOG_WARN("Failed to setup audio transcoding, will record video only");
            m_audio_stream_idx = -1;

            // 移除音频输出流
            for (unsigned i = 0; i < m_output_ctx->nb_streams; i++) {
                if (m_output_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    // 找到音频流，需要标记为无效
                    // 由于 FFmpeg 不支持删除流，我们只能保留它但忽略它
                    LOG_WARN("Audio stream left in output but will not be written");
                    break;
                }
            }
        } else {
            // 转码设置成功，将编码器参数复制到输出流
            for (unsigned i = 0; i < m_output_ctx->nb_streams; i++) {
                if (m_output_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    AVStream* out_stream = m_output_ctx->streams[i];
                    int ret = avcodec_parameters_from_context(out_stream->codecpar, m_audio_encoder_ctx);
                    if (ret < 0) {
                        LOG_ERROR("Failed to copy audio encoder parameters to output stream");
                        return false;
                    }
                    LOG_INFO("Audio encoder parameters copied to output stream");
                    break;
                }
            }
        }
    }

    // 打开输出文件（处理 Windows 中文路径问题）
#ifdef _WIN32
    // Windows: 临时切换到目标目录，使用相对路径打开文件
    std::string old_dir;
    wchar_t old_wdir[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, old_wdir)) {
        // 转换为 UTF-8
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        old_dir = converter.to_bytes(old_wdir);
    }

    // 切换到临时目录
    std::wstring temp_wdir = utf8_to_wide(m_temp_dir);
    SetCurrentDirectoryW(temp_wdir.c_str());

    // 使用相对路径打开文件
    ret = avio_open(&m_output_ctx->pb, m_current_filename.c_str(), AVIO_FLAG_WRITE);

    // 恢复原目录
    if (!old_dir.empty()) {
        SetCurrentDirectoryW(utf8_to_wide(old_dir).c_str());
    }
#else
    ret = avio_open(&m_output_ctx->pb, full_path.c_str(), AVIO_FLAG_WRITE);
#endif

    if (ret < 0) {
        LOG_ERROR("Cannot open output file: {}", av_err_to_string(ret));
        return false;
    }

    ret = avformat_write_header(m_output_ctx, nullptr);
    if (ret < 0) {
        LOG_ERROR("Cannot write header: {}", av_err_to_string(ret));
        return false;
    }

    LOG_INFO("Opened output file: {}", full_path);
    return true;
}

void IPCRecorder::closeOutput() {
    if (m_output_ctx) {
        // 刷新音频编码器中剩余的帧（如果启用了音频转码）
        if (m_audio_encoder_ctx && m_audio_fifo_initialized > 0) {
            LOG_INFO("Flushing audio encoder before closing output...");

            // 步骤1: 编码 FIFO 中剩余的完整帧
            encodeAndFlushAudioFrames();

            // 步骤2: 处理 FIFO 中不足一个完整帧的剩余采样（用静音填充）
            int remaining_samples = av_audio_fifo_size(m_audio_fifo);
            if (remaining_samples > 0) {
                LOG_INFO("Padding {} remaining audio samples with silence", remaining_samples);

                AVFrame* silence_frame = av_frame_alloc();
                silence_frame->nb_samples = m_audio_encoder_ctx->frame_size;
                silence_frame->channel_layout = m_audio_encoder_ctx->channel_layout;
                silence_frame->channels = m_audio_encoder_ctx->channels;
                silence_frame->format = m_audio_encoder_ctx->sample_fmt;
                silence_frame->sample_rate = m_audio_encoder_ctx->sample_rate;
                silence_frame->pts = m_audio_frame_count;

                int ret = av_frame_get_buffer(silence_frame, 0);
                if (ret >= 0) {
                    // 先从 FIFO 读取剩余采样
                    int read_samples = av_audio_fifo_read(m_audio_fifo, (void**)silence_frame->data, remaining_samples);
                    LOG_DEBUG("Read {} samples from FIFO", read_samples);

                    // 填充剩余位置为静音
                    int samples_to_silence = m_audio_encoder_ctx->frame_size - read_samples;
                    if (samples_to_silence > 0) {
                        // 计算每个声道的字节数
                        int bytes_per_sample = av_get_bytes_per_sample(m_audio_encoder_ctx->sample_fmt);
                        int silence_bytes = samples_to_silence * silence_frame->channels * bytes_per_sample;

                        // 对每个平面填充静音（AV_AUDIO_PLANAR 格式需要分别填充）
                        int planes = av_sample_fmt_is_planar(m_audio_encoder_ctx->sample_fmt) ?
                                    silence_frame->channels : 1;

                        for (int i = 0; i < planes; i++) {
                            memset(silence_frame->data[i] + read_samples * bytes_per_sample *
                                   (planes > 1 ? 1 : silence_frame->channels), 0, silence_bytes);
                        }
                        LOG_DEBUG("Filled {} samples with silence", samples_to_silence);
                    }

                    // 更新帧计数
                    m_audio_frame_count += m_audio_encoder_ctx->frame_size;

                    // 发送静音帧到编码器
                    ret = avcodec_send_frame(m_audio_encoder_ctx, silence_frame);
                    if (ret >= 0) {
                        // 获取编码后的包
                        while (ret >= 0) {
                            AVPacket* encoded_packet = av_packet_alloc();
                            ret = avcodec_receive_packet(m_audio_encoder_ctx, encoded_packet);
                            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                                av_packet_free(&encoded_packet);
                                break;
                            }
                            if (ret < 0) {
                                LOG_WARN("Error encoding silence frame: {}", av_err_to_string(ret));
                                av_packet_free(&encoded_packet);
                                break;
                            }

                            // 找到输出流中的音频流索引
                            int out_audio_idx = -1;
                            for (unsigned i = 0; i < m_output_ctx->nb_streams; i++) {
                                if (m_output_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                                    out_audio_idx = i;
                                    break;
                                }
                            }

                            if (out_audio_idx >= 0) {
                                encoded_packet->stream_index = out_audio_idx;
                                av_packet_rescale_ts(encoded_packet,
                                                    m_audio_encoder_ctx->time_base,
                                                    m_output_ctx->streams[out_audio_idx]->time_base);
                                writePacket(encoded_packet);
                                LOG_DEBUG("Wrote padded audio packet to stream {}", out_audio_idx);
                            }

                            av_packet_free(&encoded_packet);
                        }
                    } else {
                        LOG_WARN("Error sending silence frame to encoder: {}", av_err_to_string(ret));
                    }
                } else {
                    LOG_WARN("Failed to allocate silence frame buffer: {}", av_err_to_string(ret));
                }

                av_frame_free(&silence_frame);
            }

            // 步骤3: 发送 NULL 帧到编码器，告知没有更多输入
            LOG_DEBUG("Sending NULL frame to audio encoder to flush...");
            avcodec_send_frame(m_audio_encoder_ctx, nullptr);

            // 步骤4: 接收所有剩余的编码包（循环直到没有更多数据）
            int flush_count = 0;
            while (true) {
                AVPacket* encoded_packet = av_packet_alloc();
                int ret = avcodec_receive_packet(m_audio_encoder_ctx, encoded_packet);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_free(&encoded_packet);
                    break;
                }
                if (ret < 0) {
                    LOG_WARN("Error receiving flushed audio packet: {}", av_err_to_string(ret));
                    av_packet_free(&encoded_packet);
                    break;
                }

                // 找到输出流中的音频流索引
                int out_audio_idx = -1;
                for (unsigned i = 0; i < m_output_ctx->nb_streams; i++) {
                    if (m_output_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                        out_audio_idx = i;
                        break;
                    }
                }

                if (out_audio_idx >= 0) {
                    encoded_packet->stream_index = out_audio_idx;
                    // 只进行时间基准转换
                    av_packet_rescale_ts(encoded_packet,
                                        m_audio_encoder_ctx->time_base,
                                        m_output_ctx->streams[out_audio_idx]->time_base);
                    writePacket(encoded_packet);
                    flush_count++;
                    LOG_DEBUG("Flushed audio packet {} to stream {}", flush_count, out_audio_idx);
                }

                av_packet_free(&encoded_packet);
            }

            LOG_INFO("Audio encoder flushed, wrote {} packets", flush_count);
        }

        av_write_trailer(m_output_ctx);
        avio_closep(&m_output_ctx->pb);
        avformat_free_context(m_output_ctx);
        m_output_ctx = nullptr;

        // 移动文件从临时目录到最终目录，并重命名添加完整信息
        if (!m_current_filename.empty()) {
            // 计算实际录制时长（基于视频PTS）
            int64_t duration_seconds = 0;
            int64_t video_duration_seconds = 0;
            int64_t audio_duration_seconds = 0;

            if (m_last_video_pts > 0 && m_segment_start_pts >= 0 && m_video_stream_idx >= 0) {
                // 计算相对PTS（最后一个视频PTS - 起始PTS）
                int64_t relative_pts = m_last_video_pts - m_segment_start_pts;

                // 使用视频流的 time_base 将相对 PTS 转换为秒
                AVRational tb = m_video_time_base;
                video_duration_seconds = av_rescale_q(relative_pts, tb, AVRational{1, 1});
                duration_seconds = video_duration_seconds;
                LOG_DEBUG("Video duration calculation: last_pts={}, start_pts={}, relative_pts={}, tb={}/{}, duration={}s",
                               m_last_video_pts, m_segment_start_pts, relative_pts, tb.num, tb.den, video_duration_seconds);
            }

            // 计算音频时长（用于调试音视频同步）
            if (m_last_audio_pts > 0 && m_audio_start_pts >= 0 && m_audio_stream_idx >= 0) {
                int64_t audio_relative_pts = m_last_audio_pts - m_audio_start_pts;
                audio_duration_seconds = av_rescale_q(audio_relative_pts, m_audio_time_base, AVRational{1, 1});
                LOG_INFO("Audio duration calculation: last_pts={}, start_pts={}, relative_pts={}, tb={}/{}, duration={}s",
                              m_last_audio_pts, m_audio_start_pts, audio_relative_pts,
                              m_audio_time_base.num, m_audio_time_base.den, audio_duration_seconds);

                // 计算音视频时长差异
                if (video_duration_seconds > 0) {
                    int64_t duration_diff = video_duration_seconds - audio_duration_seconds;
                    if (duration_diff > 0) {
                        LOG_WARN("Video is {} seconds longer than audio (video: {}s, audio: {}s)",
                                 duration_diff, video_duration_seconds, audio_duration_seconds);
                    } else if (duration_diff < 0) {
                        LOG_WARN("Audio is {} seconds longer than video (audio: {}s, video: {}s)",
                                 -duration_diff, audio_duration_seconds, video_duration_seconds);
                    } else {
                        LOG_INFO("Perfect audio-video synchronization: both {}s", video_duration_seconds);
                    }
                }
            }

            // 如果视频PTS计算失败（或为0），回退到系统时间计算
            if (duration_seconds <= 0) {
                std::time_t end_time;
                std::time(&end_time);
                duration_seconds = end_time - m_segment_start_time;
                LOG_WARN("Video PTS not available, using system time for duration: {}s", duration_seconds);
            }

            // 关键帧质量检查
            if (video_duration_seconds > 0) {
                // 计算期望的关键帧数量（假设2秒一个关键帧）
                int expected_keyframes = static_cast<int>(video_duration_seconds / 2) + 1;
                LOG_INFO("Segment quality check: duration={}s, expected ~{} keyframes, file starts with key frame: YES",
                         video_duration_seconds, expected_keyframes);
            }

            // 最短时长检查：过短的分段丢弃
            if (duration_seconds < m_min_segment_duration_seconds) {
                LOG_WARN("Segment too short ({}s < {}s), discarding: {}",
                         duration_seconds, m_min_segment_duration_seconds, m_current_filename);
                std::error_code ec;
                fs::remove(fs::path(m_temp_dir) / m_current_filename, ec);
                m_current_filename.clear();
                return;
            }

            // 生成新的文件名（包含结束时间和时长）
            std::string new_filename = generateFilenameFromTemplate(m_segment_start_pts, 0, duration_seconds);

            // 确保最终目录存在
            fs::path new_filepath(new_filename);
            fs::path raw_dir = fs::path(m_output_dir) / "raw";
            if (new_filepath.has_parent_path()) {
                raw_dir /= new_filepath.parent_path();
            }
#ifdef _WIN32
            create_directories_recursive(raw_dir.string());
#else
            fs::create_directories(raw_dir);
#endif

            fs::path temp_path = fs::path(m_temp_dir) / m_current_filename;
            // TODO: 两阶段录制重构 - 输出到 raw 子目录
            fs::path final_path = fs::path(m_output_dir) / "raw" / new_filename;

            // 移动文件（使用 UTF-8 兼容函数）
#ifdef _WIN32
            if (rename_file_utf8(temp_path.string(), final_path.string())) {
                LOG_INFO("Moved recording: {} -> {}", m_current_filename, new_filename);

                // TODO: 两阶段录制重构 - 创建 CSV 日志文件
                #ifdef ENABLE_RKNN_SMART_RECORDING
                if (m_detection_logger) {
                    std::string csv_path = m_detection_logger->createLogFile(final_path);
                    if (!csv_path.empty()) {
                        LOG_INFO("Created detection log: {}", csv_path);
                    }
                }
                #endif
            } else {
                LOG_ERROR("Failed to move recording {} to {}: {}",
                               m_current_filename, new_filename, GetLastError());
            }
#else
            std::error_code ec;
            if (fs::exists(temp_path)) {
                fs::rename(temp_path, final_path, ec);
                if (!ec) {
                    LOG_INFO("Moved recording: {} -> {}", m_current_filename, new_filename);

                    // TODO: 两阶段录制重构 - 重命名 CSV 日志文件到最终路径
                    #ifdef ENABLE_RKNN_SMART_RECORDING
                    if (m_detection_logger) {
                        // 获取临时CSV文件路径（在temp_dir中）
                        fs::path temp_csv = fs::path(m_temp_dir) / m_current_filename;
                        temp_csv.replace_extension(".csv");

                        // 创建最终CSV文件路径（在raw目录，使用最终文件名）
                        fs::path final_csv = fs::path(m_output_dir) / "raw" / new_filename;
                        final_csv.replace_extension(".csv");

                        // 重命名临时CSV为最终CSV
                        if (fs::exists(temp_csv)) {
                            std::error_code ec;
                            fs::rename(temp_csv, final_csv, ec);
                            if (!ec) {
                                LOG_INFO("Moved detection log: {} -> {}", temp_csv.filename().string(), final_csv.filename().string());
                            } else {
                                LOG_WARN("Failed to move detection log: {} -> {}, error: {}",
                                        temp_csv.string(), final_csv.string(), ec.message());
                            }
                        } else {
                            LOG_DEBUG("Temp CSV not found (may be empty): {}", temp_csv.string());
                        }

                        // 完成最终CSV文件的写入
                        m_detection_logger->finalizeLogFile(final_csv.string());
                    }
                    #endif
                } else {
                    LOG_ERROR("Failed to move recording {} to {}: {}",
                                   m_current_filename, new_filename, ec.message().c_str());
                }
            } else {
                LOG_WARN("Temp file not found: {}", temp_path.string());
            }
#endif

            m_current_filename.clear();
        }
    }
}

bool IPCRecorder::setupStreams() {
    for (unsigned i = 0; i < m_input_ctx->nb_streams; i++) {
        if (m_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_video_stream_idx = i;
        } else if (m_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_enable_audio) {
            m_audio_stream_idx = i;
            // 保存音频时间基准
            m_audio_time_base = m_input_ctx->streams[i]->time_base;
            LOG_DEBUG("Audio time base: {}/{}", m_audio_time_base.num, m_audio_time_base.den);
        }
    }

    if (m_video_stream_idx == -1) {
        LOG_ERROR("No video stream found");
        return false;
    }

    if (!m_enable_audio) {
        LOG_INFO("Audio recording disabled by configuration");
    }

    return true;
}

bool IPCRecorder::setupAudioTranscoding() {
    if (m_audio_stream_idx == -1) {
        LOG_INFO("No audio stream found, video only mode");
        return true;
    }

    AVStream* audio_stream = m_input_ctx->streams[m_audio_stream_idx];
    AVCodecParameters* audio_par = audio_stream->codecpar;

    LOG_INFO("Found audio stream: codec={}, sample_rate={}, channels={}",
                   avcodec_get_name(audio_par->codec_id), audio_par->sample_rate,
                   audio_par->channels);

    // 检查是否需要转码（已在 openOutput 中检查）
    // 这里直接获取目标编码器并设置转码
    AVCodecID target_codec_id = getBestAudioCodec(m_output_format);

    // 如果已经是目标编码器，不需要转码
    if (audio_par->codec_id == target_codec_id) {
        LOG_INFO("Audio codec is already {}, no transcoding needed", avcodec_get_name(target_codec_id));
        m_audio_decoder_ctx = nullptr;
        m_audio_encoder_ctx = nullptr;
        return true;
    }

    LOG_INFO("Audio transcoding: {} -> {} (format: {})",
                   avcodec_get_name(audio_par->codec_id), avcodec_get_name(target_codec_id), m_output_format);

    // 尝试使用目标编码器，如果失败则回退到 AAC
    AVCodecID codecs_to_try[] = {target_codec_id, AV_CODEC_ID_AAC};
    const char* codec_names[] = {avcodec_get_name(target_codec_id), "AAC"};

    for (int attempt = 0; attempt < 2; attempt++) {
        AVCodecID try_codec = codecs_to_try[attempt];

        // 跳过重复的编码器
        if (attempt > 0 && try_codec == codecs_to_try[attempt - 1]) {
            continue;
        }

        LOG_INFO("Attempting to use {} encoder", codec_names[attempt]);

        // 查找编码器
        const AVCodec* encoder = avcodec_find_encoder(try_codec);
        if (!encoder) {
            LOG_WARN("{} encoder not found", codec_names[attempt]);
            continue;
        }

        // 创建音频编码器上下文
        m_audio_encoder_ctx = avcodec_alloc_context3(encoder);
        if (!m_audio_encoder_ctx) {
            LOG_WARN("Failed to allocate audio encoder context for {}", codec_names[attempt]);
            continue;
        }

        // 设置编码器参数
        m_audio_encoder_ctx->sample_rate = audio_par->sample_rate > 0 ? audio_par->sample_rate : 44100;
        m_audio_encoder_ctx->channels = audio_par->channels > 0 ? audio_par->channels : 1;
        m_audio_encoder_ctx->channel_layout = audio_par->channel_layout;
        if (m_audio_encoder_ctx->channel_layout == 0) {
            m_audio_encoder_ctx->channel_layout = av_get_default_channel_layout(m_audio_encoder_ctx->channels);
        }
        m_audio_encoder_ctx->sample_fmt = encoder->sample_fmts ? encoder->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

        // 设置比特率（根据采样率和通道数调整）
        int channels = m_audio_encoder_ctx->channels;
        if (try_codec == AV_CODEC_ID_AAC) {
            // AAC: 根据采样率和通道数设置合理的比特率
            // 8000 Hz: 32-64 kbps per channel
            // 16000 Hz: 48-96 kbps per channel
            // 44100/48000 Hz: 128-192 kbps per channel
            if (m_audio_encoder_ctx->sample_rate <= 8000) {
                m_audio_encoder_ctx->bit_rate = channels * 32000;
            } else if (m_audio_encoder_ctx->sample_rate <= 16000) {
                m_audio_encoder_ctx->bit_rate = channels * 64000;
            } else {
                m_audio_encoder_ctx->bit_rate = channels * 128000;
            }
        } else {
            m_audio_encoder_ctx->bit_rate = 128000;
        }

        // 设置 frame_size（AAC 需要）
        if (encoder->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE) {
            m_audio_encoder_ctx->frame_size = 1;  // 可变帧大小
        } else if (try_codec == AV_CODEC_ID_AAC) {
            // AAC 固定帧大小：通常是 1024 个样本
            m_audio_encoder_ctx->frame_size = 1024;
        }

        // 设置时间基准
        m_audio_encoder_ctx->time_base = AVRational{1, m_audio_encoder_ctx->sample_rate};

        // 打开编码器
        int ret = avcodec_open2(m_audio_encoder_ctx, encoder, nullptr);
        if (ret < 0) {
            LOG_WARN("Failed to open {} encoder: {}, trying next codec", codec_names[attempt], av_err_to_string(ret));
            avcodec_free_context(&m_audio_encoder_ctx);
            m_audio_encoder_ctx = nullptr;
            continue;
        }

        LOG_INFO("Successfully opened {} encoder", codec_names[attempt]);

        // 成功，跳出循环
        break;
    }

    // 如果所有编码器都失败
    if (!m_audio_encoder_ctx) {
        LOG_ERROR("Failed to open any audio encoder, transcoding aborted");
        return false;
    }

    // 查找输入音频解码器
    const AVCodec* decoder = avcodec_find_decoder(audio_par->codec_id);
    if (!decoder) {
        LOG_ERROR("Audio decoder not found for codec={}", avcodec_get_name(audio_par->codec_id));
        return false;
    }

    // 创建解码器上下文
    m_audio_decoder_ctx = avcodec_alloc_context3(decoder);
    if (!m_audio_decoder_ctx) {
        LOG_ERROR("Failed to allocate audio decoder context");
        return false;
    }

    // 复制解码器参数
    int ret = avcodec_parameters_to_context(m_audio_decoder_ctx, audio_par);
    if (ret < 0) {
        LOG_ERROR("Failed to copy audio decoder parameters: {}", av_err_to_string(ret));
        return false;
    }

    // 打开解码器
    ret = avcodec_open2(m_audio_decoder_ctx, decoder, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open audio decoder: {}", av_err_to_string(ret));
        return false;
    }

    LOG_INFO("Audio transcoding setup completed: {} -> {}",
                   avcodec_get_name(audio_par->codec_id), avcodec_get_name(m_audio_encoder_ctx->codec_id));

    return true;
}

bool IPCRecorder::shouldSwitchSegment(const AVPacket* packet) {
    if (!m_output_ctx) {
        return false;
    }

    // 只在视频关键帧上检查分段切换
    if (packet->stream_index == m_video_stream_idx && (packet->flags & AV_PKT_FLAG_KEY)) {
        int64_t pts_diff = packet->pts - m_segment_start_pts;
        int64_t duration_in_sec = av_rescale_q(pts_diff, m_video_time_base, AVRational{1, 1});

        if (duration_in_sec >= m_segment_duration) {
            // 如果有音频流，检查音频是否也接近分段时长
            if (m_audio_stream_idx >= 0 && m_last_audio_pts > 0) {
                int64_t audio_duration_in_sec = av_rescale_q(
                    m_last_audio_pts - m_audio_start_pts,
                    m_audio_time_base,
                    AVRational{1, 1}
                );

                // 允许音频比视频晚最多2秒（避免无限等待）
                int64_t max_audio_delay = 2;
                if (audio_duration_in_sec < duration_in_sec - max_audio_delay) {
                    LOG_DEBUG("Waiting for audio to catch up: video={}s, audio={}s",
                             duration_in_sec, audio_duration_in_sec);
                    return false;  // 音频落后太多，等待音频追赶
                }
            }
            return true;
        }
    }

    return false;
}

std::string IPCRecorder::generateFilename(int64_t start_pts, int64_t end_pts) {
    int64_t start_sec = av_rescale_q(start_pts, m_video_time_base, AVRational{1, 1});
    auto start_time = std::chrono::system_clock::time_point() + std::chrono::seconds(start_sec);
    auto start_t = std::chrono::system_clock::to_time_t(start_time);

    std::stringstream ss;
    ss << "stream_";

    auto tm_start = *std::localtime(&start_t);
    ss << std::put_time(&tm_start, "%Y%m%d_%H%M%S");

    if (end_pts > 0) {
        int64_t end_sec = av_rescale_q(end_pts, m_video_time_base, AVRational{1, 1});
        auto end_time = std::chrono::system_clock::time_point() + std::chrono::seconds(end_sec);
        auto end_t = std::chrono::system_clock::to_time_t(end_time);
        auto tm_end = *std::localtime(&end_t);
        ss << "-" << std::put_time(&tm_end, "%H%M%S");
    }

    ss << ".mp4";
    return ss.str();
}

bool IPCRecorder::writePacket(AVPacket* packet) {
    if (!m_output_ctx) {
        LOG_ERROR("writePacket: m_output_ctx is null");
        return false;
    }

    int ret = av_interleaved_write_frame(m_output_ctx, packet);
    if (ret < 0) {
        LOG_ERROR("Error writing packet: {}", av_err_to_string(ret));
        return false;
    }
    return true;
}

// ============================================================
// 音频转码辅助函数
// ============================================================

bool IPCRecorder::initAudioResampleAndFifo() {
    if (m_audio_fifo_initialized) {
        return true;  // 已经初始化过了
    }

    // 确保 channel_layout 有效
    uint64_t dst_layout = m_audio_encoder_ctx->channel_layout;
    if (dst_layout == 0) {
        dst_layout = av_get_default_channel_layout(m_audio_encoder_ctx->channels);
    }
    uint64_t src_layout = m_audio_decoder_ctx->channel_layout;
    if (src_layout == 0) {
        src_layout = av_get_default_channel_layout(m_audio_decoder_ctx->channels);
    }

    // 初始化重采样器
    m_swr_ctx = swr_alloc_set_opts(m_swr_ctx,
                                  dst_layout,
                                  m_audio_encoder_ctx->sample_fmt,
                                  m_audio_encoder_ctx->sample_rate,
                                  src_layout,
                                  m_audio_decoder_ctx->sample_fmt,
                                  m_audio_decoder_ctx->sample_rate,
                                  0, nullptr);

    if (!m_swr_ctx || swr_init(m_swr_ctx) < 0) {
        LOG_ERROR("Failed to initialize resampler");
        return false;
    }

    // 创建 FIFO 缓冲区
    m_audio_fifo = av_audio_fifo_alloc(m_audio_encoder_ctx->sample_fmt,
                                      m_audio_encoder_ctx->channels,
                                      1);
    if (!m_audio_fifo) {
        LOG_ERROR("Failed to allocate audio FIFO");
        return false;
    }

    m_audio_fifo_initialized = 1;
    return true;
}

bool IPCRecorder::resampleAndStoreAudioFrame(AVFrame* frame) {
    // 分配重采样后的数据缓冲区
    uint8_t** converted_data = nullptr;
    int aligned_samples = av_samples_alloc_array_and_samples(&converted_data, nullptr,
                                                             m_audio_encoder_ctx->channels,
                                                             frame->nb_samples,
                                                             m_audio_encoder_ctx->sample_fmt,
                                                             0);
    if (aligned_samples < 0) {
        LOG_ERROR("Failed to allocate converted samples");
        return false;
    }

    // 重采样到临时缓冲
    int out_samples = swr_convert(m_swr_ctx,
                                 converted_data,
                                 frame->nb_samples,
                                 (const uint8_t**)frame->data,
                                 frame->nb_samples);

    if (out_samples < 0) {
        LOG_ERROR("Error resampling audio");
        av_freep(&converted_data[0]);
        av_freep(&converted_data);
        return false;
    }

    // 扩展 FIFO 缓冲区
    if (av_audio_fifo_realloc(m_audio_fifo, av_audio_fifo_size(m_audio_fifo) + out_samples) < 0) {
        LOG_ERROR("Failed to reallocate audio FIFO");
        av_freep(&converted_data[0]);
        av_freep(&converted_data);
        return false;
    }

    // 写入 FIFO
    if (av_audio_fifo_write(m_audio_fifo, (void**)converted_data, out_samples) != out_samples) {
        LOG_ERROR("Failed to write to audio FIFO");
        av_freep(&converted_data[0]);
        av_freep(&converted_data);
        return false;
    }

    // 清理临时缓冲区
    av_freep(&converted_data[0]);
    av_freep(&converted_data);
    return true;
}

bool IPCRecorder::encodeAndFlushAudioFrames() {
    // 从 FIFO 读取足够的样本进行编码
    while (av_audio_fifo_size(m_audio_fifo) >= m_audio_encoder_ctx->frame_size) {
        AVFrame* enc_frame = av_frame_alloc();
        enc_frame->nb_samples = m_audio_encoder_ctx->frame_size;
        enc_frame->channel_layout = m_audio_encoder_ctx->channel_layout;
        enc_frame->channels = m_audio_encoder_ctx->channels;
        enc_frame->format = m_audio_encoder_ctx->sample_fmt;
        enc_frame->sample_rate = m_audio_encoder_ctx->sample_rate;

        int ret = av_frame_get_buffer(enc_frame, 0);
        if (ret < 0) {
            LOG_ERROR("Failed to allocate encoder frame buffer: {}", av_err_to_string(ret));
            av_frame_free(&enc_frame);
            return false;
        }

        int read_samples = av_audio_fifo_read(m_audio_fifo, (void**)enc_frame->data, m_audio_encoder_ctx->frame_size);
        if (read_samples != m_audio_encoder_ctx->frame_size) {
            LOG_WARN("Incomplete read from audio FIFO: {} != {}", read_samples, m_audio_encoder_ctx->frame_size);
        }

        // 设置 PTS（使用累积的帧数）
        enc_frame->pts = m_audio_frame_count;
        m_audio_frame_count += m_audio_encoder_ctx->frame_size;

        // 发送到编码器
        ret = avcodec_send_frame(m_audio_encoder_ctx, enc_frame);
        av_frame_free(&enc_frame);  // 释放帧，编码器已经保留了一份拷贝

        if (ret < 0) {
            LOG_ERROR("Error sending frame to audio encoder: {}", av_err_to_string(ret));
            return false;
        }

        // 获取编码后的包
        while (ret >= 0) {
            AVPacket* encoded_packet = av_packet_alloc();
            ret = avcodec_receive_packet(m_audio_encoder_ctx, encoded_packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&encoded_packet);
                break;
            }
            if (ret < 0) {
                LOG_ERROR("Error encoding audio frame: {}", av_err_to_string(ret));
                av_packet_free(&encoded_packet);
                break;
            }

            // 找到输出流中的音频流索引
            int out_audio_idx = -1;
            for (unsigned i = 0; i < m_output_ctx->nb_streams; i++) {
                if (m_output_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                    out_audio_idx = i;
                    break;
                }
            }

            if (out_audio_idx >= 0) {
                encoded_packet->stream_index = out_audio_idx;
                // 使用编码器返回的 PTS/DTS（已经基于 m_audio_frame_count，是单调递增的）
                // 只进行时间基准转换
                av_packet_rescale_ts(encoded_packet,
                                    m_audio_encoder_ctx->time_base,
                                    m_output_ctx->streams[out_audio_idx]->time_base);
                writePacket(encoded_packet);
            }

            av_packet_free(&encoded_packet);
        }
    }
    return true;
}

bool IPCRecorder::decodeAndProcessAudioPackets(AVPacket* packet) {
    // 接收解码后的帧（循环处理，因为一个包可能产生多个帧）
    AVFrame* frame = av_frame_alloc();
    int ret = 0;

    while (ret >= 0) {
        ret = avcodec_receive_frame(m_audio_decoder_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;  // 没有更多帧了
        }
        if (ret < 0) {
            LOG_ERROR("Error decoding audio frame: {}", av_err_to_string(ret));
            av_frame_free(&frame);
            return false;
        }

        // 重采样并存储到 FIFO
        if (!resampleAndStoreAudioFrame(frame)) {
            av_frame_unref(frame);  // 清理帧
            av_frame_free(&frame);
            return false;
        }

        // 清理帧以准备下一次接收
        av_frame_unref(frame);
    }

    av_frame_free(&frame);

    // 尝试编码并刷新 FIFO 中的数据
    return encodeAndFlushAudioFrames();
}

void IPCRecorder::resetAudioTranscodingState() {
    // 清空 FIFO 缓冲区
    if (m_audio_fifo) {
        av_audio_fifo_reset(m_audio_fifo);
        LOG_DEBUG("Audio FIFO reset for new segment");
    }

    // 重置帧计数器
    m_audio_frame_count = 0;
}

// ============================================================
// 主音频转码函数
// ============================================================

bool IPCRecorder::transcodeAudio(AVPacket* packet) {
    // 1. 验证编码器上下文
    if (!m_audio_decoder_ctx || !m_audio_encoder_ctx) {
        return false;
    }

    // 2. 验证 packet 有效性
    if (!packet) {
        LOG_WARN("Null packet passed to transcodeAudio");
        return false;
    }

    // 3. 验证数据指针和大小
    if (!packet->data || packet->size <= 0) {
        LOG_DEBUG("Skipping empty audio packet: data={}, size={}",
                       static_cast<void*>(packet->data), packet->size);
        return false;  // 空包不是错误，跳过即可
    }

    // 4. 验证包大小是否合理
    if (packet->size > 8192) {
        LOG_WARN("Audio packet size too large: {}", packet->size);
        return false;
    }

    // 5. 发送包到解码器
    int ret = avcodec_send_packet(m_audio_decoder_ctx, packet);
    if (ret == AVERROR(EAGAIN)) {
        // 解码器缓冲区满，先取出已解码的帧再重试
        if (!m_audio_fifo_initialized && !initAudioResampleAndFifo()) {
            return false;
        }
        decodeAndProcessAudioPackets(nullptr);
        ret = avcodec_send_packet(m_audio_decoder_ctx, packet);
    }
    if (ret < 0) {
        LOG_ERROR("Error sending audio packet to decoder: {}", av_err_to_string(ret));
        return false;
    }

    // 6. 初始化重采样器和 FIFO（如果需要）
    if (!m_audio_fifo_initialized && !initAudioResampleAndFifo()) {
        return false;
    }

    // 7. 解码并处理音频包
    return decodeAndProcessAudioPackets(packet);
}

std::string IPCRecorder::formatDate(std::time_t time, const std::string& format) {
    std::tm tm = *std::localtime(&time);
    std::ostringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

std::string IPCRecorder::formatDuration(int64_t seconds) {
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    int secs = seconds % 60;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(2) << hours
       << std::setfill('0') << std::setw(2) << minutes
       << std::setfill('0') << std::setw(2) << secs;
    return ss.str();
}

std::string IPCRecorder::getVideoCodecName() {
    if (m_video_stream_idx < 0 || !m_input_ctx) {
        return "unknown";
    }
    AVCodecParameters* codec_par = m_input_ctx->streams[m_video_stream_idx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codec_par->codec_id);
    if (codec) {
        std::string name = codec->name;
        // 转换为大写
        std::transform(name.begin(), name.end(), name.begin(), ::toupper);
        return name;
    }
    return "UNKNOWN";
}

int IPCRecorder::getVideoWidth() {
    if (m_video_stream_idx < 0 || !m_input_ctx) {
        return 0;
    }
    return m_input_ctx->streams[m_video_stream_idx]->codecpar->width;
}

int IPCRecorder::getVideoHeight() {
    if (m_video_stream_idx < 0 || !m_input_ctx) {
        return 0;
    }
    return m_input_ctx->streams[m_video_stream_idx]->codecpar->height;
}

double IPCRecorder::getVideoFPS() {
    if (m_video_stream_idx < 0 || !m_input_ctx) {
        return 0.0;
    }
    AVRational frame_rate = m_input_ctx->streams[m_video_stream_idx]->avg_frame_rate;
    if (frame_rate.den > 0) {
        return static_cast<double>(frame_rate.num) / frame_rate.den;
    }
    return 0.0;
}

std::string IPCRecorder::generateUUID() {
    // 生成短格式的 UUID（8个字符）
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);

    std::ostringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) {
        ss << dis(gen);
    }
    return ss.str();
}

std::string IPCRecorder::generateFilenameFromTemplate(int64_t start_pts, int64_t end_pts, int64_t duration_seconds) {
    std::string result = m_filename_template;

    // 获取全局序列号
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(m_global_mutex);
        sequence = m_global_sequence++;
    }

    // 计算变量值
    std::map<std::string, std::string> vars;

    // 流相关
    vars["{stream_id}"] = m_stream_id;
    vars["{stream_name}"] = m_stream_name;
    vars["{shop_id}"] = std::to_string(m_shop_id);

    // 时间相关 — 基于 PTS 锚点计算，确保分片间时间戳连续无重叠
    std::time_t seg_start_time;
    if (m_stream_start_wallclock > 0 && m_segment_start_pts >= m_stream_start_pts) {
        int64_t pts_offset = m_segment_start_pts - m_stream_start_pts;
        int64_t secs = av_rescale_q(pts_offset, m_video_time_base, AVRational{1, 1});
        seg_start_time = m_stream_start_wallclock + secs;
    } else {
        seg_start_time = m_segment_start_time;  // 回退：锚点未设置时用系统墙钟
    }

    vars["{start_date}"] = formatDate(seg_start_time, "%Y-%m-%d");
    vars["{start_time}"] = formatDate(seg_start_time, "%H%M%S");
    vars["{start_datetime}"] = formatDate(seg_start_time, "%Y%m%d_%H%M%S");

    std::time_t end_time = seg_start_time + duration_seconds;
    vars["{end_time}"] = formatDate(end_time, "%H%M%S");
    vars["{end_datetime}"] = formatDate(end_time, "%Y%m%d_%H%M%S");

    // 时长相关
    vars["{duration_seconds}"] = std::to_string(duration_seconds);
    vars["{duration}"] = formatDuration(duration_seconds);

    // 分段相关
    vars["{segment_index}"] = std::to_string(m_segment_index);
    vars["{segment_global_index}"] = std::to_string(sequence);

    // 视频信息
    vars["{width}"] = std::to_string(getVideoWidth());
    vars["{height}"] = std::to_string(getVideoHeight());
    vars["{codec}"] = getVideoCodecName();

    std::ostringstream fps_ss;
    fps_ss << std::fixed << std::setprecision(2) << getVideoFPS();
    vars["{fps}"] = fps_ss.str();

    // 其他
    vars["{sequence}"] = std::to_string(sequence);
    vars["{uuid}"] = generateUUID();

    // 主机名
    char hostname[256] = {0};
#ifdef _WIN32
    DWORD size = sizeof(hostname);
    GetComputerNameA(hostname, &size);
#else
    gethostname(hostname, sizeof(hostname));
#endif
    vars["{hostname}"] = hostname;

    // 替换所有变量
    for (const auto& var : vars) {
        size_t pos = 0;
        while ((pos = result.find(var.first, pos)) != std::string::npos) {
            result.replace(pos, var.first.length(), var.second);
            pos += var.second.length();
        }
    }

    return result;
}

IPCRecorder::StatusInfo IPCRecorder::getStatus() const {
    StatusInfo info;
    info.is_recording = m_running.load() && m_output_ctx != nullptr;
    info.reconnect_count = m_reconnect_count.load();
    info.last_error = m_last_error;
    info.last_packet_time = m_last_packet_time.load();
    return info;
}

std::string IPCRecorder::parseOutputFormat() {
    // 验证格式是否被 FFmpeg 支持
    const AVOutputFormat* fmt = av_guess_format(nullptr, m_filename_template.c_str(), nullptr);
    if (fmt) {
        return fmt->name;
    }

    // 默认使用 mp4
    return "mp4";
}

bool IPCRecorder::isVideoCodecCompatible(AVCodecID codec_id, const std::string& format_name) {
    // 使用 FFmpeg API 查询格式是否支持该视频编码
    const AVOutputFormat* fmt = av_guess_format(format_name.c_str(), nullptr, nullptr);
    if (!fmt) {
        LOG_WARN("Unknown output format: {}", format_name);
        return false;
    }

    // 使用 avformat_query_codec 检查兼容性
    int ret = avformat_query_codec(fmt, codec_id, FF_COMPLIANCE_NORMAL);
    // 返回 1 表示支持，0 表示不支持，负值表示错误
    return ret == 1;
}

bool IPCRecorder::isAudioCodecCompatible(AVCodecID codec_id, const std::string& format_name) {
    // 使用 FFmpeg API 查询格式是否支持该音频编码
    const AVOutputFormat* fmt = av_guess_format(format_name.c_str(), nullptr, nullptr);
    if (!fmt) {
        LOG_WARN("Unknown output format: {}", format_name);
        return false;
    }

    // 使用 avformat_query_codec 检查兼容性
    int ret = avformat_query_codec(fmt, codec_id, FF_COMPLIANCE_NORMAL);
    return ret == 1;
}

AVCodecID IPCRecorder::getBestAudioCodec(const std::string& format_name) {
    // 使用 FFmpeg API 查找格式的最佳音频编码器
    const AVOutputFormat* fmt = av_guess_format(format_name.c_str(), nullptr, nullptr);
    if (!fmt || !fmt->audio_codec) {
        // 默认使用 AAC
        return AV_CODEC_ID_AAC;
    }

    // 返回格式推荐的音频编码器
    return fmt->audio_codec;
}
