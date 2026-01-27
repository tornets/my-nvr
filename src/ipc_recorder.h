//
// Created by wention on 2026/1/27.
//

#ifndef NVR_IPC_RECORDER_H
#define NVR_IPC_RECORDER_H

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

class IPCRecorder {
public:
    IPCRecorder(const std::string& stream_url, const std::string& output_dir);
    ~IPCRecorder();

    void start();
    void stop();

private:
    void recordingLoop();
    bool openInput();
    bool openOutput(const std::string& filename);
    void closeOutput();
    bool setupStreams();
    bool setupAudioTranscoding();
    bool shouldSwitchSegment(const AVPacket* packet);
    std::string generateFilename(int64_t start_pts, int64_t end_pts);
    bool writePacket(AVPacket* packet);
    bool transcodeAudio(AVPacket* packet);

    std::string m_stream_url;
    std::string m_output_dir;
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::mutex m_mutex;

    AVFormatContext* m_input_ctx;
    AVFormatContext* m_output_ctx;

    int m_video_stream_idx;
    int m_audio_stream_idx;

    AVCodecContext* m_audio_decoder_ctx;
    AVCodecContext* m_audio_encoder_ctx;

    int64_t m_segment_start_pts;
    int64_t m_segment_duration;
    AVRational m_video_time_base;
    int64_t m_current_dts;
    int64_t m_pts_offset;

    std::shared_ptr<spdlog::logger> m_logger;
};


#endif //NVR_IPC_RECORDER_H
