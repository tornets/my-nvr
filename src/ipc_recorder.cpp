//
// Created by wention on 2026/1/27.
//

#include "ipc_recorder.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

IPCRecorder::IPCRecorder(const std::string& stream_url, const std::string& output_dir, int segment_duration)
    : m_stream_url(stream_url)
    , m_output_dir(output_dir)
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
    , m_audio_start_pts(0)
    , m_segment_duration(segment_duration)
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
    m_logger->info("Starting recording: {}", m_stream_url);

    if (!openInput()) {
        m_logger->error("Failed to open input stream");
        return;
    }

    if (!setupStreams()) {
        m_logger->error("Failed to setup streams");
        return;
    }

    if (m_audio_stream_idx != -1 && !setupAudioTranscoding()) {
        m_logger->warn("Failed to setup audio transcoding, will record video only");
        m_audio_stream_idx = -1;
    }

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

                // 重置音频起始 PTS 和帧计数
                m_audio_start_pts = 0;
                m_audio_frame_count = 0;
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
        } else if (packet->stream_index == m_audio_stream_idx && m_output_ctx) {
            // 处理音频包
            if (m_audio_start_pts == 0 && packet->pts > 0) {
                // 记录第一个音频包的 PTS 作为起始点
                m_audio_start_pts = packet->pts;
                m_logger->debug("Audio start PTS set to: {}", m_audio_start_pts);
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
        } else if (in_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && m_audio_stream_idx != -1) {
            // 添加音频流到输出
            AVStream* out_stream = avformat_new_stream(m_output_ctx, nullptr);
            if (!out_stream) {
                m_logger->error("Failed to allocate audio output stream");
                return false;
            }

            // 如果有音频编码器，使用编码器的参数；否则复制输入参数
            if (m_audio_encoder_ctx) {
                ret = avcodec_parameters_from_context(out_stream->codecpar, m_audio_encoder_ctx);
                if (ret < 0) {
                    m_logger->error("Failed to copy audio encoder parameters");
                    return false;
                }
            } else {
                ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
                if (ret < 0) {
                    m_logger->error("Failed to copy audio codec parameters");
                    return false;
                }
            }

            out_stream->time_base = in_stream->time_base;
            out_stream->codecpar->codec_tag = 0;

            m_logger->info("Added audio stream to output");
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
    if (m_audio_stream_idx == -1) {
        m_logger->info("No audio stream found, video only mode");
        return true;
    }

    AVStream* audio_stream = m_input_ctx->streams[m_audio_stream_idx];
    AVCodecParameters* audio_par = audio_stream->codecpar;

    m_logger->info("Found audio stream: codec={}, sample_rate={}, channels={}",
                   avcodec_get_name(audio_par->codec_id), audio_par->sample_rate,
                   audio_par->ch_layout.nb_channels);

    // 检查音频编码格式，如果是 AAC 则不需要转码
    if (audio_par->codec_id == AV_CODEC_ID_AAC) {
        m_logger->info("Audio codec is AAC, no transcoding needed");
        m_audio_decoder_ctx = nullptr;
        m_audio_encoder_ctx = nullptr;
        return true;
    }

    // 需要转码为 AAC
    m_logger->info("Audio transcoding to AAC enabled");

    // 查找 AAC 编码器
    const AVCodec* aac_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!aac_codec) {
        m_logger->error("AAC encoder not found");
        return false;
    }

    // 创建音频编码器上下文
    m_audio_encoder_ctx = avcodec_alloc_context3(aac_codec);
    if (!m_audio_encoder_ctx) {
        m_logger->error("Failed to allocate audio encoder context");
        return false;
    }

    // 设置编码器参数
    m_audio_encoder_ctx->sample_rate = audio_par->sample_rate > 0 ? audio_par->sample_rate : 44100;
    m_audio_encoder_ctx->ch_layout = audio_par->ch_layout;
    if (m_audio_encoder_ctx->ch_layout.nb_channels == 0) {
        av_channel_layout_default(&m_audio_encoder_ctx->ch_layout, 2);
    }
    m_audio_encoder_ctx->sample_fmt = aac_codec->sample_fmts ? aac_codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

    // 设置比特率
    m_audio_encoder_ctx->bit_rate = 128000;

    // 设置时间基准
    m_audio_encoder_ctx->time_base = AVRational{1, m_audio_encoder_ctx->sample_rate};

    // 打开编码器
    int ret = avcodec_open2(m_audio_encoder_ctx, aac_codec, nullptr);
    if (ret < 0) {
        m_logger->error("Failed to open AAC encoder: {}", av_err2str(ret));
        return false;
    }

    // 查找输入音频解码器
    const AVCodec* decoder = avcodec_find_decoder(audio_par->codec_id);
    if (!decoder) {
        m_logger->error("Audio decoder not found for codec={}", avcodec_get_name(audio_par->codec_id));
        return false;
    }

    // 创建解码器上下文
    m_audio_decoder_ctx = avcodec_alloc_context3(decoder);
    if (!m_audio_decoder_ctx) {
        m_logger->error("Failed to allocate audio decoder context");
        return false;
    }

    // 复制解码器参数
    ret = avcodec_parameters_to_context(m_audio_decoder_ctx, audio_par);
    if (ret < 0) {
        m_logger->error("Failed to copy audio decoder parameters: {}", av_err2str(ret));
        return false;
    }

    // 打开解码器
    ret = avcodec_open2(m_audio_decoder_ctx, decoder, nullptr);
    if (ret < 0) {
        m_logger->error("Failed to open audio decoder: {}", av_err2str(ret));
        return false;
    }

    m_logger->info("Audio transcoding setup completed: {} -> AAC",
                   avcodec_get_name(audio_par->codec_id));

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
    if (!m_audio_decoder_ctx || !m_audio_encoder_ctx) {
        return false;
    }

    // 发送包到解码器
    int ret = avcodec_send_packet(m_audio_decoder_ctx, packet);
    if (ret < 0) {
        m_logger->error("Error sending audio packet to decoder: {}", av_err2str(ret));
        return false;
    }

    // 初始化重采样器和 FIFO 缓冲区
    if (!m_audio_fifo_initialized) {
        // 初始化重采样器
        ret = swr_alloc_set_opts2(&m_swr_ctx,
                                 &m_audio_encoder_ctx->ch_layout,
                                 m_audio_encoder_ctx->sample_fmt,
                                 m_audio_encoder_ctx->sample_rate,
                                 &m_audio_decoder_ctx->ch_layout,
                                 m_audio_decoder_ctx->sample_fmt,
                                 m_audio_decoder_ctx->sample_rate,
                                 0, nullptr);

        if (!m_swr_ctx || swr_init(m_swr_ctx) < 0) {
            m_logger->error("Failed to initialize resampler");
            return false;
        }

        // 创建 FIFO 缓冲区
        m_audio_fifo = av_audio_fifo_alloc(m_audio_encoder_ctx->sample_fmt,
                                          m_audio_encoder_ctx->ch_layout.nb_channels,
                                          1);
        if (!m_audio_fifo) {
            m_logger->error("Failed to allocate audio FIFO");
            return false;
        }

        m_audio_fifo_initialized = 1;
    }

    // 保存输入包的 PTS（在时间基准转换前）
    int64_t input_pts = packet->pts;
    AVRational input_tb = m_input_ctx->streams[m_audio_stream_idx]->time_base;

    // 接收解码后的帧
    AVFrame* frame = av_frame_alloc();
    while (ret >= 0) {
        ret = avcodec_receive_frame(m_audio_decoder_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            m_logger->error("Error decoding audio frame: {}", av_err2str(ret));
            av_frame_free(&frame);
            return false;
        }

        // 重采样
        AVFrame* output_frame = av_frame_alloc();
        output_frame->nb_samples = m_audio_encoder_ctx->frame_size;
        output_frame->ch_layout = m_audio_encoder_ctx->ch_layout;
        output_frame->format = m_audio_encoder_ctx->sample_fmt;
        output_frame->sample_rate = m_audio_encoder_ctx->sample_rate;
        output_frame->pts = frame->pts;  // 保留原始 PTS

        av_frame_get_buffer(output_frame, 0);

        int out_samples = swr_convert(m_swr_ctx,
                                     output_frame->data,
                                     output_frame->nb_samples,
                                     (const uint8_t**)frame->data,
                                     frame->nb_samples);

        if (out_samples < 0) {
            m_logger->error("Error resampling audio");
            av_frame_free(&frame);
            av_frame_free(&output_frame);
            return false;
        }

        // 编码
        ret = avcodec_send_frame(m_audio_encoder_ctx, output_frame);
        if (ret < 0) {
            m_logger->error("Error sending frame to audio encoder: {}", av_err2str(ret));
            av_frame_free(&frame);
            av_frame_free(&output_frame);
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
                m_logger->error("Error encoding audio frame: {}", av_err2str(ret));
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

                // 根据输入帧的 PTS 重新计算输出包的 PTS
                // 将输入 PTS 转换为输出时间基准，然后减去起始 PTS
                int64_t pts_rescaled = av_rescale_q(input_pts, input_tb,
                                                    m_audio_encoder_ctx->time_base);
                encoded_packet->pts = pts_rescaled - m_audio_start_pts;
                encoded_packet->dts = encoded_packet->pts;

                // 再转换到输出流的时间基准
                av_packet_rescale_ts(encoded_packet,
                                    m_audio_encoder_ctx->time_base,
                                    m_output_ctx->streams[out_audio_idx]->time_base);
                writePacket(encoded_packet);
            }

            av_packet_free(&encoded_packet);
        }

        av_frame_free(&output_frame);
    }

    av_frame_free(&frame);
    return true;
}
