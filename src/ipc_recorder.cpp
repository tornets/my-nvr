//
// Created by wention on 2026/1/27.
//

#include "ipc_recorder.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

IPCRecorder::IPCRecorder(const std::string& stream_url, const std::string& output_dir)
    : m_stream_url(stream_url)
    , m_output_dir(output_dir)
    , m_running(false)
    , m_input_ctx(nullptr)
    , m_output_ctx(nullptr)
    , m_video_stream_idx(-1)
    , m_audio_stream_idx(-1)
    , m_audio_decoder_ctx(nullptr)
    , m_audio_encoder_ctx(nullptr)
    , m_segment_start_pts(0)
    , m_segment_duration(600)
    , m_video_time_base{1, 90000}
    , m_current_dts(0)
    , m_pts_offset(0)
{
    fs::create_directories(m_output_dir);
    m_logger = spdlog::get("recorder");
    if (!m_logger) {
        m_logger = spdlog::default_logger()->clone("recorder");
    }
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
    if (m_thread.joinable()) {
        m_thread.join();
    }

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
    }

    if (m_audio_encoder_ctx) {
        avcodec_free_context(&m_audio_encoder_ctx);
    }
}

void IPCRecorder::recordingLoop() {
    m_logger->info("Starting recording: {}", m_stream_url);

    if (!openInput()) {
        m_logger->error("Failed to open input stream");
        return;
    }

    if (!setupStreams()) {
        m_logger->error("Failed to setup streams");
        return;
    }

    m_logger->info("Skipping audio transcoding setup (video only mode)");

    AVPacket* packet = av_packet_alloc();

    while (m_running) {
        int ret = av_read_frame(m_input_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                m_logger->info("End of stream");
            } else {
                m_logger->error("Error reading frame: {}", av_err2str(ret));
            }
            break;
        }

        if (packet->pts < 0 || (m_output_ctx == nullptr && !(packet->flags & AV_PKT_FLAG_KEY)))
            continue;

        if (m_output_ctx) {
            auto ts = (double)av_rescale_q(packet->pts - m_segment_start_pts, m_output_ctx->streams[0]->time_base, AV_TIME_BASE_Q)/ AV_TIME_BASE;
            if (ts > 10 && packet->flags & AV_PKT_FLAG_KEY) {
                closeOutput();
            }

            m_logger->debug("Writing packet: original_pts={}, original_dts={}, ts={}",
                            packet->pts, packet->dts, ts);
        }

        if (packet->stream_index == m_video_stream_idx) {
            if (!m_output_ctx) {
                std::time_t rawtime;
                std::time(&rawtime);
                // Convert to local time structure
                std::tm* timeinfo = std::localtime(&rawtime);

                char buf[80];
                // Format time into "YYYY-MM-DD HH:MM:SS"
                std::strftime(buf, sizeof(buf), "stream_%Y%m%d_%H%M%S.mp4", timeinfo);
                std::string filename = buf;
                m_logger->info("Opening output file: {}", filename);
                if (!openOutput(filename)) {
                    m_logger->error("Failed to open output file: {}", filename);
                    av_packet_unref(packet);
                    continue;
                }
                m_logger->info("Output file opened successfully");

                if (packet->pts > 0)
                    m_segment_start_pts = packet->pts;
            }

            int stream_index = packet->stream_index;

            if (packet->flags & AV_PKT_FLAG_KEY) {
                m_logger->debug("this packet contain key frame");
            }

            packet->pts -= m_segment_start_pts;
            packet->dts -= m_segment_start_pts;
            //av_packet_rescale_ts(packet, m_input_ctx->streams[stream_index]->time_base,
            //                    m_output_ctx->streams[0]->time_base);

            writePacket(packet);
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    closeOutput();

    m_logger->info("Recording stopped: {}", m_stream_url);
}

bool IPCRecorder::openInput() {
    int ret = avformat_open_input(&m_input_ctx, m_stream_url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        m_logger->error("Cannot open input: {}", av_err2str(ret));
        return false;
    }

    ret = avformat_find_stream_info(m_input_ctx, nullptr);
    if (ret < 0) {
        m_logger->error("Cannot find stream info: {}", av_err2str(ret));
        return false;
    }

    return true;
}

bool IPCRecorder::openOutput(const std::string& filename) {
    std::string full_path = (fs::path(m_output_dir) / filename).string();

    int ret = avformat_alloc_output_context2(&m_output_ctx, nullptr, "mp4", full_path.c_str());
    if (ret < 0) {
        m_logger->error("Cannot create output context: {}", av_err2str(ret));
        return false;
    }

    for (unsigned i = 0; i < m_input_ctx->nb_streams; i++) {
        AVStream* in_stream = m_input_ctx->streams[i];

        if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVStream* out_stream = avformat_new_stream(m_output_ctx, nullptr);
            if (!out_stream) {
                m_logger->error("Failed to allocate output stream");
                return false;
            }

            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                m_logger->error("Failed to copy codec parameters");
                return false;
            }

            m_video_time_base = in_stream->time_base;
            out_stream->time_base = in_stream->time_base;
            out_stream->codecpar->codec_tag = 0;
        }
    }

    ret = avio_open(&m_output_ctx->pb, full_path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) {
        m_logger->error("Cannot open output file: {}", av_err2str(ret));
        return false;
    }

    ret = avformat_write_header(m_output_ctx, nullptr);
    if (ret < 0) {
        m_logger->error("Cannot write header: {}", av_err2str(ret));
        return false;
    }

    m_logger->info("Opened output file: {}, using pts_offset: {}", full_path, m_pts_offset);
    return true;
}

void IPCRecorder::closeOutput() {
    if (m_output_ctx) {
        av_write_trailer(m_output_ctx);
        avio_closep(&m_output_ctx->pb);
        avformat_free_context(m_output_ctx);
        m_output_ctx = nullptr;
    }
}

bool IPCRecorder::setupStreams() {
    for (unsigned i = 0; i < m_input_ctx->nb_streams; i++) {
        if (m_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_video_stream_idx = i;
        } else if (m_input_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audio_stream_idx = i;
        }
    }

    if (m_video_stream_idx == -1) {
        m_logger->error("No video stream found");
        return false;
    }

    return true;
}

bool IPCRecorder::setupAudioTranscoding() {
    m_logger->info("Audio transcoding disabled - video only mode");
    m_audio_stream_idx = -1;
    return true;
}

bool IPCRecorder::shouldSwitchSegment(const AVPacket* packet) {
    if (!m_output_ctx) {
        return false;
    }

    int64_t pts_diff = packet->pts - m_segment_start_pts;
    int64_t duration_in_sec = av_rescale_q(pts_diff, m_video_time_base, AVRational{1, 1});

    if (duration_in_sec >= m_segment_duration) {
        if (packet->flags & AV_PKT_FLAG_KEY) {
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
        m_logger->error("writePacket: m_output_ctx is null");
        return false;
    }

    int ret = av_interleaved_write_frame(m_output_ctx, packet);
    if (ret < 0) {
        m_logger->error("Error writing packet: {}", av_err2str(ret));
        return false;
    }
    return true;
}

bool IPCRecorder::transcodeAudio(AVPacket* packet) {
    AVPacket* out_packet = av_packet_clone(packet);
    if (!out_packet) {
        return false;
    }

    av_packet_rescale_ts(out_packet, m_input_ctx->streams[packet->stream_index]->time_base,
                        m_output_ctx->streams[1]->time_base);
    writePacket(out_packet);
    av_packet_free(&out_packet);
    return true;
}
