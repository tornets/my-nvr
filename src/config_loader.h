#pragma once

#include <string>
#include <vector>
#include <optional>
#include <map>

struct StreamConfig {
    std::string id;
    std::string url;
    std::map<std::string, std::string> extra_params;  // 额外参数，如 timeout=30
    bool auto_reconnect;           // 是否自动重连
    int reconnect_interval_seconds; // 重连间隔（秒）
    int max_reconnect_attempts;     // 最大重连尝试次数（-1表示无限重连）
    int timeout_seconds;            // 流超时时间（秒），无数据超过此时长视为离线
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
