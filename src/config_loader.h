#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>

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

// RKNN 检测配置
struct RKNNConfig {
    bool enabled = false;                      // 是否启用 RKNN 检测
    std::string model_path = "";               // RKNN 模型路径
    int detection_interval_keyframes = 30;     // 检测间隔（关键帧数）
    int player_class_id = 0;                   // 玩家类别 ID
    int npc_class_id = 1;                      // NPC 类别 ID
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
};

struct ShopConfig {
    int id;                       // 店铺ID
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
};

struct Config {
    std::vector<StreamConfig> streams;
    std::string log_level;         // 日志级别: trace, debug, info, warn, error, critical
    AutoCleanConfig autoclean;     // 自动清理配置
    UploadConfig upload;           // 上传配置
    RecordConfig record;           // 录制配置
    ShopConfig shop;               // 店铺配置

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
