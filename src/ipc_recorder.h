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
#include <libavutil/audio_fifo.h>
#include <libswresample/swresample.h>
}

class IPCRecorder {
public:
    IPCRecorder(const std::string& stream_id, const std::string& stream_url,
                const std::string& output_dir, const std::string& temp_dir,
                int segment_duration = 600,
                const std::string& filename_template = "{stream_id}_{start_datetime}_seg{segment_index}_{duration}.mp4");
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
    std::string generateFilenameFromTemplate(int64_t start_pts, int64_t end_pts, int64_t duration_seconds);
    bool writePacket(AVPacket* packet);
    bool transcodeAudio(AVPacket* packet);

    // 模板解析辅助方法
    std::string formatDate(std::time_t time, const std::string& format);
    std::string formatDuration(int64_t seconds);
    std::string getVideoCodecName();
    int getVideoWidth();
    int getVideoHeight();
    double getVideoFPS();
    std::string generateUUID();

    std::string m_stream_id;
    std::string m_stream_url;
    std::string m_output_dir;
    std::string m_temp_dir;          // 临时文件目录
    std::string m_filename_template;
    std::string m_current_filename;  // 当前录制的文件名
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::mutex m_mutex;

    AVFormatContext* m_input_ctx;
    AVFormatContext* m_output_ctx;

    int m_video_stream_idx;
    int m_audio_stream_idx;

    AVCodecContext* m_audio_decoder_ctx;
    AVCodecContext* m_audio_encoder_ctx;

    // 音频转码相关
    AVAudioFifo* m_audio_fifo;
    SwrContext* m_swr_ctx;
    int m_audio_fifo_initialized;
    int64_t m_audio_frame_count;  // 用于计算音频输出 PTS

    int64_t m_segment_start_pts;
    int64_t m_audio_start_pts;  // 音频流起始 PTS
    int64_t m_segment_duration;
    int m_segment_index;         // 当前分段序号（每个流独立）
    std::time_t m_segment_start_time; // 分段开始时间
    AVRational m_video_time_base;
    int64_t m_current_dts;
    int64_t m_pts_offset;

    // 全局计数器
    static std::atomic<uint64_t> m_global_sequence;      // 全局序列号
    static std::mutex m_global_mutex;

    std::shared_ptr<spdlog::logger> m_logger;
};


#endif //NVR_IPC_RECORDER_H
