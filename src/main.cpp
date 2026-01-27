extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/imgutils.h>
}

#include <iostream>

struct StreamTSConfig {
    AVStream* input;
    AVStream* output;
};

struct VideoTransCodeTask {
    char* input_filename;
    char* output_filename;

    AVFormatContext* input_fmt_ctx;
    AVFormatContext* output_fmt_ctx;

    StreamTSConfig streams;
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " input.mp4\n";
        return -1;
    }

    const char* url = argv[1];
    const char* filename = argv[2];
    AVFormatContext* fmt_ctx = nullptr;
    AVFormatContext* out_fmt_ctx = nullptr;

    // 1. 打开文件
    if (avformat_open_input(&fmt_ctx, url, nullptr, nullptr) < 0) {
        std::cerr << "Could not open input file\n";
        return -1;
    }

    if (avformat_alloc_output_context2(&out_fmt_ctx, nullptr, "mp4", filename) < 0) {
        std::cerr << "Could not create output context" << std::endl;
        return -1;
    }

    // 2. 读取流信息
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Could not find stream info\n";
        return -1;
    }

    // 3. 找到视频流
    int video_stream_index = -1;
    int audio_stream_index = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;

            auto out_stream = avformat_new_stream(out_fmt_ctx, NULL);
            if (!out_stream) {
                std::cerr << "Failed to create new stream" << std::endl;
                return 1;
            }
            avcodec_parameters_copy(out_stream->codecpar, fmt_ctx->streams[i]->codecpar);
        }

        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;

            auto out_stream = avformat_new_stream(out_fmt_ctx, NULL);
            if (!out_stream) {
                std::cerr << "Failed to create new stream" << std::endl;
                return 1;
            }
            avcodec_parameters_copy(out_stream->codecpar, fmt_ctx->streams[i]->codecpar);
        }
    }

    auto audio_decoder = avcodec_find_decoder(fmt_ctx->streams[audio_stream_index]->codecpar->codec_id);
    if (!audio_decoder) {
        fprintf(stderr, "Could not find decoder for stream #%d\n", audio_stream_index);
        return 1;
    }

    auto audio_decoder_ctx = avcodec_alloc_context3(audio_decoder);
    if (!audio_decoder_ctx) {
        std::cerr << "Could not allocate audio decoder context." << std::endl;
        return 1;
    }

    if (avcodec_parameters_to_context(audio_decoder_ctx, fmt_ctx->streams[audio_stream_index]->codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters." << std::endl;
        return 1;
    }

    if (avcodec_open2(audio_decoder_ctx, audio_decoder, nullptr) < 0) {
        std::cerr << "Failed to open codec." << std::endl;
        return 1;
    }

    auto audio_encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audio_encoder) {
        fprintf(stderr, "Could not find aac encoder\n");
        return 1;
    }

    auto audio_encoder_ctx = avcodec_alloc_context3(audio_encoder);
    if (!audio_encoder_ctx) {
        std::cerr << "Could not allocate output codec context." << std::endl;
        return 1;
    }

    // 设置输出音频流的参数
    av_channel_layout_copy(&audio_encoder_ctx->ch_layout, &audio_encoder_ctx->ch_layout);
    audio_encoder_ctx->sample_rate = audio_decoder_ctx->sample_rate;
    audio_encoder_ctx->sample_fmt = audio_decoder_ctx->sample_fmt;
    audio_encoder_ctx->bit_rate = audio_decoder_ctx->bit_rate;

    if (avcodec_open2(audio_encoder_ctx, audio_encoder, nullptr) < 0) {
        std::cerr << "Could not open output codec." << std::endl;
        return 1;
    }

    // 打开输出文件
    if (avio_open(&out_fmt_ctx->pb, filename, AVIO_FLAG_WRITE) < 0) {
        std::cerr << "Could not open output file" << std::endl;
        return 1;
    }

    // 写入文件头
    if (avformat_write_header(out_fmt_ctx, nullptr) < 0) {
        std::cerr << "Error writing file header" << std::endl;
        return 1;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // 6. 读取并解码
    int cnt = 0;
    while (av_read_frame(fmt_ctx, packet) >= 0) {
        int stream_index = packet->stream_index;

        // 假设 pkt 已经被读取
        double pts_in_sec =
                (double) av_rescale_q(packet->pts, fmt_ctx->streams[stream_index]->time_base, AV_TIME_BASE_Q) /
                AV_TIME_BASE;

        printf("PTS in seconds: %f\n", pts_in_sec);

        if (pts_in_sec > 25) {
            av_packet_unref(packet);
            break;
        }

        if (packet->stream_index == video_stream_index) {
            av_packet_rescale_ts(packet, fmt_ctx->streams[stream_index]->time_base, out_fmt_ctx->streams[0]->time_base);
            if (av_interleaved_write_frame(out_fmt_ctx, packet) < 0) {
                std::cerr << "Error writing frame to output file" << std::endl;
                av_packet_unref(packet);
                break;
            }
        } else if (packet->stream_index == audio_stream_index) {
            int error;
            int data_present = 0;
            if ((error = avcodec_send_packet(audio_decoder_ctx, packet)) < 0) {
                fprintf(stderr, "Could not send packet for decoding (error '%s')\n",
                        av_err2str(error));
                av_packet_unref(packet);
                return 1;
            }

            /* Receive one frame from the decoder. */
            error = avcodec_receive_frame(audio_decoder_ctx, frame);
            /* If the decoder asks for more data to be able to decode a frame,
             * return indicating that no data is present. */
            if (error == AVERROR(EAGAIN)) {
                error = 0;
                /* If the end of the input file is reached, stop decoding. */
            } else if (error == AVERROR_EOF) {
                error = 0;
                break;
            } else if (error < 0) {
                fprintf(stderr, "Could not decode frame (error '%s')\n",
                        av_err2str(error));
                break;
                /* Default case: Return decoded data. */
            } else {
                data_present = 1;
            }

        }

        av_packet_unref(packet);
        cnt++;
    }


    av_write_trailer(out_fmt_ctx);
    avformat_free_context(out_fmt_ctx);

    // 7. 释放资源
    av_frame_free(&frame);
    av_packet_free(&packet);
    avformat_close_input(&fmt_ctx);

    return 0;
}
