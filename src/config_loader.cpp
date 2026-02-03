#include "config_loader.h"
#include <yaml-cpp/yaml.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <unistd.h>
#include <pwd.h>
#endif

Config Config::getDefault() {
    Config config;
    config.streams.clear();
    config.log_level = "debug";

    // 录制默认配置
    config.record.output_dir = "./recordings";
    config.record.temp_dir = "./recordings/.temp";  // 临时目录
    config.record.segment_duration_seconds = 600;  // 10分钟
    config.record.filename_template = "{stream_id}_{start_datetime}_seg{segment_index}_{duration}.mp4";
    config.record.enable_audio = true;  // 默认启用音频录制

    // 自动清理默认配置
    config.autoclean.enabled = true;                 // 默认启用
    config.autoclean.max_age_hours = 168;            // 7 days
    config.autoclean.max_disk_usage_gb = 100;
    config.autoclean.check_interval_seconds = 3;

    // 上传默认配置
    config.upload.enabled = false;                   // 默认禁用
    config.upload.url = "";                          // 空字符串表示禁用上传
    config.upload.timeout_seconds = 300;
    config.upload.max_retries = 3;
    config.upload.retry_delay_seconds = 5;

    // 店铺默认配置
    config.shop.id = 0;                              // 默认店铺ID为0

    return config;
}

std::optional<std::string> Config::findConfigFile() {
    // 搜索路径列表
    std::vector<std::string> search_paths = {
        "./config.yaml",
        "./config.yml"
    };

#ifdef _WIN32
    // Windows: 添加用户配置目录
    char app_data_path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, app_data_path))) {
        search_paths.push_back(std::string(app_data_path) + "\\nvr\\config.yaml");
        search_paths.push_back(std::string(app_data_path) + "\\nvr\\config.yml");
    }
#else
    // Linux/Unix: 添加用户主目录和 /etc
    const char* home = getenv("HOME");
    if (home) {
        search_paths.push_back(std::string(home) + "/.nvr/config.yaml");
        search_paths.push_back(std::string(home) + "/.nvr/config.yml");
    }
    search_paths.push_back("/etc/nvr/config.yaml");
    search_paths.push_back("/etc/nvr/config.yml");
#endif

    // 按顺序搜索
    for (const auto& path : search_paths) {
        std::ifstream file(path);
        if (file.good()) {
            file.close();
            return path;
        }
    }

    return std::nullopt;
}

std::optional<Config> Config::fromYaml(const std::string& filepath) {
    try {
        // 检查文件是否存在
        std::ifstream file(filepath);
        if (!file.good()) {
            return std::nullopt;
        }
        file.close();

        // 加载YAML文件
        YAML::Node yaml = YAML::LoadFile(filepath);

        Config config = getDefault();

        // 解析流配置
        if (yaml["streams"]) {
            const auto& streams_node = yaml["streams"];
            if (streams_node.IsSequence()) {
                for (const auto& stream : streams_node) {
                    StreamConfig stream_config;

                    if (stream["id"]) {
                        stream_config.id = stream["id"].as<std::string>();
                    } else {
                        std::cerr << "Warning: stream missing 'id', using default" << std::endl;
                        stream_config.id = "camera_" + std::to_string(config.streams.size() + 1);
                    }

                    if (stream["url"]) {
                        stream_config.url = stream["url"].as<std::string>();
                    } else {
                        std::cerr << "Warning: stream '" << stream_config.id << "' missing 'url', skipping" << std::endl;
                        continue;
                    }

                    // 解析额外参数
                    if (stream["params"]) {
                        const auto& params = stream["params"];
                        if (params.IsMap()) {
                            for (const auto& param : params) {
                                stream_config.extra_params[param.first.as<std::string>()] =
                                    param.second.as<std::string>();
                            }
                        }
                    }

                    config.streams.push_back(stream_config);
                }
            }
        }

        // 解析录制配置
        if (yaml["record"]) {
            const auto& record = yaml["record"];
            if (record["output_dir"]) {
                config.record.output_dir = record["output_dir"].as<std::string>();
            }
            if (record["temp_dir"]) {
                config.record.temp_dir = record["temp_dir"].as<std::string>();
            }
            if (record["segment_duration_seconds"]) {
                config.record.segment_duration_seconds = record["segment_duration_seconds"].as<int>();
            }
            if (record["filename_template"]) {
                config.record.filename_template = record["filename_template"].as<std::string>();
            }
            if (record["enable_audio"]) {
                config.record.enable_audio = record["enable_audio"].as<bool>();
            }
        }

        // 解析自动清理配置
        if (yaml["autoclean"]) {
            const auto& autoclean = yaml["autoclean"];
            if (autoclean["enabled"]) {
                config.autoclean.enabled = autoclean["enabled"].as<bool>();
            }
            if (autoclean["max_age_hours"]) {
                config.autoclean.max_age_hours = autoclean["max_age_hours"].as<int>();
            }
            if (autoclean["max_disk_usage_gb"]) {
                config.autoclean.max_disk_usage_gb = autoclean["max_disk_usage_gb"].as<int>();
            }
            if (autoclean["check_interval_seconds"]) {
                config.autoclean.check_interval_seconds = autoclean["check_interval_seconds"].as<int>();
            }
        }

        // 解析日志级别
        if (yaml["log_level"]) {
            config.log_level = yaml["log_level"].as<std::string>();
        }

        // 解析上传配置
        if (yaml["upload"]) {
            const auto& upload = yaml["upload"];
            if (upload["enabled"]) {
                config.upload.enabled = upload["enabled"].as<bool>();
            }
            if (upload["url"]) {
                config.upload.url = upload["url"].as<std::string>();
            }
            if (upload["timeout_seconds"]) {
                config.upload.timeout_seconds = upload["timeout_seconds"].as<int>();
            }
            if (upload["max_retries"]) {
                config.upload.max_retries = upload["max_retries"].as<int>();
            }
            if (upload["retry_delay_seconds"]) {
                config.upload.retry_delay_seconds = upload["retry_delay_seconds"].as<int>();
            }
        }

        // 解析店铺配置
        if (yaml["shop"]) {
            const auto& shop = yaml["shop"];
            if (shop["id"]) {
                config.shop.id = shop["id"].as<int>();
            }
        }

        return config;

    } catch (const YAML::Exception& e) {
        std::cerr << "Failed to parse YAML: " << e.what() << std::endl;
        return std::nullopt;
    }
}

std::optional<StreamConfig> Config::parseStreamArgument(const std::string& arg) {
    StreamConfig config;

    // 检查是否包含 url=（向后兼容纯URL的情况）
    if (arg.find("url=") == std::string::npos) {
        // 可能是纯URL，向后兼容
        config.id = "camera1";
        config.url = arg;
        return config;
    }

    // 解析 streamid,url=xxx[,key=value]*
    std::istringstream iss(arg);
    std::string token;

    // 第一部分应该是 streamid
    if (!std::getline(iss, token, ',')) {
        std::cerr << "Invalid stream argument: " << arg << std::endl;
        return std::nullopt;
    }
    config.id = token;

    // 其余部分是 key=value 对
    bool has_url = false;
    while (std::getline(iss, token, ',')) {
        size_t eq_pos = token.find('=');
        if (eq_pos == std::string::npos) {
            std::cerr << "Invalid parameter format: " << token << " (expected key=value)" << std::endl;
            continue;
        }

        std::string key = token.substr(0, eq_pos);
        std::string value = token.substr(eq_pos + 1);

        if (key == "url") {
            config.url = value;
            has_url = true;
        } else {
            config.extra_params[key] = value;
        }
    }

    if (!has_url) {
        std::cerr << "Stream '" << config.id << "' missing url parameter" << std::endl;
        return std::nullopt;
    }

    return config;
}

void Config::mergeStream(const StreamConfig& stream) {
    // 查找是否已存在相同ID的流
    for (auto& existing : streams) {
        if (existing.id == stream.id) {
            // 覆盖已存在的流
            existing = stream;
            return;
        }
    }

    // 添加新流
    streams.push_back(stream);
}
