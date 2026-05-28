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

#include "log.h"

#ifdef ENABLE_RKNN_SMART_RECORDING
#include "config_loader.h"
#include "detection_pool.h"
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// 前向声明
class DetectionLogger;

// 前向声明
class IPCRecorder;

#ifdef ENABLE_RKNN_SMART_RECORDING
namespace nvr {
class SmartRecordingManager;
}
#endif

// 中断回调函数（用于超时检测）
int interrupt_callback(void* ctx);

class IPCRecorder {
    friend int ::interrupt_callback(void* ctx);

public:
    IPCRecorder(const std::string& stream_id, const std::string& stream_url,
                const std::string& output_dir, const std::string& temp_dir,
                int segment_duration = 600,
                const std::string& filename_template = "{stream_id}_{start_datetime}_seg{segment_index}_{duration}.mp4",
                bool enable_audio = true,
                bool auto_reconnect = true,
                int reconnect_interval_seconds = 5,
                int max_reconnect_attempts = -1,
                int timeout_seconds = 30,
                int shop_id = 0,
                const std::string& stream_name = ""
#ifdef ENABLE_RKNN_SMART_RECORDING
                , const SmartRecordingConfig* smart_recording_config = nullptr
                , nvr::detection::DetectionPool* detection_pool = nullptr
#endif
    );
    ~IPCRecorder();

    void start();
    void stop();

    // 连接并录制（内部使用）
    bool connectAndRecord();

    // 获取录制状态信息
    struct StatusInfo {
        bool is_recording;
        int reconnect_count;
        std::string last_error;
        int64_t last_packet_time;
    };
    StatusInfo getStatus() const;

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

    // 音频转码辅助方法
    bool initAudioResampleAndFifo();
    bool decodeAndProcessAudioPackets(AVPacket* packet);
    bool resampleAndStoreAudioFrame(AVFrame* frame);
    bool encodeAndFlushAudioFrames();
    void resetAudioTranscodingState();

    // 模板解析辅助方法
    std::string formatDate(std::time_t time, const std::string& format);
    std::string formatDuration(int64_t seconds);
    std::string getVideoCodecName();
    int getVideoWidth();
    int getVideoHeight();
    double getVideoFPS();
    std::string generateUUID();

    // 编码器兼容性检查
    bool isVideoCodecCompatible(AVCodecID codec_id, const std::string& format_name);
    bool isAudioCodecCompatible(AVCodecID codec_id, const std::string& format_name);
    AVCodecID getBestAudioCodec(const std::string& format_name);
    std::string parseOutputFormat();

    std::string m_stream_id;
    std::string m_stream_name;       // 流名称（用于文件名）
    std::string m_stream_url;
    std::string m_output_dir;
    std::string m_temp_dir;          // 临时文件目录
    std::string m_filename_template;
    std::string m_current_filename;  // 当前录制的文件名
    std::string m_output_format;     // 输出格式（mp4, mkv, avi 等）
    bool m_enable_audio;             // 是否启用音频录制
    int m_shop_id;                   // 店铺 ID

    // 重连配置
    bool m_auto_reconnect;           // 是否自动重连
    int m_reconnect_interval_seconds; // 重连间隔（秒）
    int m_max_reconnect_attempts;    // 最大重连尝试次数
    int m_timeout_seconds;           // 超时时间（秒）

    // 重连状态
    std::atomic<int> m_reconnect_count;      // 当前重连次数
    std::atomic<int64_t> m_last_packet_time; // 最后一个包的时间戳
    std::string m_last_error;                // 最后一次错误信息
    std::atomic<int64_t> m_last_read_time;   // 最后一次读取时间（用于中断回调）

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
    int64_t m_segment_start_dts;    // 视频流起始 DTS
    int64_t m_audio_start_pts;  // 音频流起始 PTS
    int64_t m_segment_duration;
    int m_segment_index;         // 当前分段序号（每个流独立）
    std::time_t m_segment_start_time; // 分段开始时间（系统时间）
    std::time_t m_stream_start_wallclock; // 录制流首次启动时的墙钟（PTS锚点）
    int64_t m_stream_start_pts; // 录制流首个分段的起始 PTS（PTS锚点）
    int64_t m_last_video_pts;   // 最后一个视频包的 PTS（用于计算实际时长）
    int64_t m_last_video_dts;   // 最后一个视频包的 DTS
    int64_t m_last_audio_pts;   // 最后一个音频包的 PTS（用于音视频同步）
    AVRational m_video_time_base;
    AVRational m_audio_time_base;  // 音频流时间基准
    int64_t m_current_dts;
    int64_t m_pts_offset;

    // 全局计数器
    static std::atomic<uint64_t> m_global_sequence;      // 全局序列号
    static std::mutex m_global_mutex;

#ifdef ENABLE_RKNN_SMART_RECORDING
    // 智能录制管理器
    std::unique_ptr<nvr::SmartRecordingManager> m_smart_recording;
    bool m_smart_recording_enabled;                     // 是否启用智能录制
    int64_t m_last_detection_pts;                       // 上次检测的 PTS

    // 硬件解码器（仅用于检测，不影响 stream copy 录制）
    AVCodecContext* m_video_decoder_ctx;
    AVFrame* m_decoded_frame;

    // 检测结果日志器
    std::unique_ptr<DetectionLogger> m_detection_logger;

    bool initHardwareDecoder();
    void closeHardwareDecoder();
    AVFrame* decodeVideoFrame(AVPacket* packet);
#endif

    std::shared_ptr<spdlog::logger> m_logger;
};


#endif //NVR_IPC_RECORDER_H
