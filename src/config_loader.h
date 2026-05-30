#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>
#include "detection_types.h"

// 时间段配置（支持跨午夜）
struct TimeRange {
    int start_hour = 0;       // 开始小时 (0-23)
    int start_minute = 0;     // 开始分钟 (0-59)
    int end_hour = 0;         // 结束小时 (0-23)
    int end_minute = 0;       // 结束分钟 (0-59)

    // 检查指定时间是否在时间段内（支持跨午夜，如 22:00-02:00）
    bool contains(int hour, int minute) const;
};

// 调度配置
struct ScheduleConfig {
    bool enabled = false;                      // 是否启用调度
    int check_interval_seconds = 60;           // 调度检查间隔（秒）
    std::vector<TimeRange> time_ranges;        // 录制时间段列表
};

// 检测模式
enum class DetectionMode {
    Keyframe,   // 只检测关键帧，按关键帧间隔跳帧
    Sampled,    // 按 PTS 时间间隔检查
    Realtime    // 每一帧都检测
};

// RKNN 检测配置
struct RKNNConfig {
    bool enabled = false;                      // 是否启用 RKNN 检测
    DetectionMode detection_mode = DetectionMode::Keyframe;  // 检测模式
    std::string model_path = "";               // RKNN 模型路径
    int detection_interval_keyframes = 30;     // keyframe 模式：检测间隔（关键帧数）
    float detection_interval_seconds = 2.0f;   // sampled 模式：检测间隔（秒）
    int player_class_id = 1;                   // 玩家类别 ID
    int npc_class_id = 0;                      // NPC 类别 ID
    float confidence_threshold = 0.5f;         // 置信度阈值
    bool zero_copy_enabled = true;             // 零拷贝开关
};

// 智能录制配置
struct SmartRecordingConfig {
    bool enabled = false;                      // 是否启用智能录制
    int prebuffer_duration_seconds = 5;        // 预缓存时长（秒）
    int segment_duration_seconds = 60;         // 分段时长（秒）
    int min_recording_duration = 10;           // 最小录制时长（秒）
    int post_recording_delay_seconds = 5;      // 玩家消失后延迟停止秒数
    RKNNConfig rknn;                           // RKNN 检测配置
    nvr::detection::DumpDetectConfig dump_detect;  // 调试图像导出配置
};

struct StreamConfig {
    std::string id;
    std::string name;               // 流名称（可选，用于文件名显示）
    std::string url;
    std::map<std::string, std::string> extra_params;  // 额外参数，如 timeout=30
    bool auto_reconnect;           // 是否自动重连
    int reconnect_interval_seconds; // 重连间隔（秒）
    int max_reconnect_attempts;     // 最大重连尝试次数（-1表示无限重连）
    int timeout_seconds;            // 流超时时间（秒），无数据超过此时长视为离线
    SmartRecordingConfig smart_recording;  // 智能录制配置
};

struct AutoCleanConfig {
    bool enabled;                 // 是否启用自动清理
    int max_age_hours;            // 最大文件保留时间（小时）
    int max_disk_usage_gb;        // 最大磁盘使用量（GB）
    int check_interval_seconds;   // 清理检查间隔（秒）
};

struct UploadConfig {
    bool enabled;                 // 是否启用上传
    std::string url;              // 上传服务器 URL
    int timeout_seconds;          // 上传超时时间
    int max_retries;              // 最大重试次数
    int retry_delay_seconds;      // 重试延迟
    int threads;                  // 上传线程数（默认 1）
    bool persist_progress;        // 是否持久化上传进度
    std::string progress_file;    // 上传进度文件路径
    std::string upload_subdir = "filter";     // 上传哪个子目录（raw/filter/all）
};

struct ShopConfig {
    int id;                       // 店铺ID
};

struct DetectionPoolConfig {
    int workers = 3;              // NPU worker 数量（对应 NPU 核心数）
    int queue_size = 64;          // 最大排队任务数（建议 >= 流数量 × 2）
};

struct UploadTask {
    std::string file_path;
    std::string stream_id;
    std::string recording_time;  // 录制时间范围
};

struct RecordConfig {
    std::string output_dir;       // 录制文件保存目录
    std::string temp_dir;         // 录制过程中的临时文件目录
    int segment_duration_seconds; // 录制分段时长（秒）
    std::string filename_template; // 文件名模板，支持变量: {stream_id}, {start_datetime}, {segment_index} 等
    bool enable_audio;            // 是否启用音频录制
    ScheduleConfig schedule;      // 录制时间段配置

    // 两阶段录制配置
    std::string raw_subdir = "raw";           // 实时录制子目录
    std::string filter_subdir = "filter";     // 事件提取子目录
    bool enable_extraction = true;            // 是否启用事件提取
    int extraction_scan_interval_seconds = 1; // 提取扫描间隔（秒）
    bool delete_raw_after_extraction = false; // 提取完成后是否删除原始视频
    int min_player_segment_duration = 5;      // 最小玩家片段时长（秒）
    int player_segment_merge_gap = 5;         // 玩家片段合并最大间隔（秒）
};

struct Config {
    std::vector<StreamConfig> streams;
    std::string log_level;         // 日志级别: trace, debug, info, warn, error, critical
    bool console_log = false;      // 是否启用控制台日志
    AutoCleanConfig autoclean;     // 自动清理配置
    UploadConfig upload;           // 上传配置
    RecordConfig record;           // 录制配置
    ShopConfig shop;               // 店铺配置
    DetectionPoolConfig detection_pool;  // NPU 检测池配置

    // 获取默认配置
    static Config getDefault();

    // 从YAML文件加载
    static std::optional<Config> fromYaml(const std::string& filepath);

    // 按路径列表搜索配置文件
    // 搜索顺序：./config.yaml -> ./config.yml -> ~/.nvr/config.yaml -> /etc/nvr/config.yaml
    static std::optional<std::string> findConfigFile();

    // 解析流参数字符串，格式：streamid,url=xxx[,key=value]*
    // 例如：camera1,url=rtsp://host/stream,timeout=30,reconnect=auto
    static std::optional<StreamConfig> parseStreamArgument(const std::string& arg);

    // 合并配置：将命令行指定的流合并到配置中
    // 如果流ID已存在，则覆盖；否则添加新流
    void mergeStream(const StreamConfig& stream);
};
