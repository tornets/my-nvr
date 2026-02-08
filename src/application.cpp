#include "application.h"
#include "nvr_manager.h"
#include "video_uploader.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <csignal>
#include <iostream>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

// Global pointers for signal handler
static std::atomic<bool>* g_running_ptr = nullptr;
static NVRManager* g_manager_ptr = nullptr;

void ApplicationState::shutdown() {
    running = false;
    if (manager) {
        manager->stopAll();
    }
    if (uploader) {
        uploader->stop();
    }
}

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        spdlog::info("Received signal, shutting down...");
        if (g_running_ptr) {
            *g_running_ptr = false;
        }
        if (g_manager_ptr) {
            g_manager_ptr->stopAll();
        }
    }
}

// 从 RTSP URL 中提取 IP 地址
static std::string extractIpFromUrl(const std::string& url) {
    // RTSP URL 格式: rtsp://username:password@192.168.1.1:554/path
    // 或者: rtsp://192.168.1.1/path
    size_t at_pos = url.find('@');
    size_t start = url.find("://");

    if (start == std::string::npos) {
        return "";
    }

    start += 3;  // 跳过 "://"

    // 如果有 @ 符号，IP 在 @ 之后
    if (at_pos != std::string::npos && at_pos > start) {
        start = at_pos + 1;
    }

    // 查找 IP 结束位置（: 或 /）
    size_t end = url.find(':', start);
    if (end == std::string::npos) {
        end = url.find('/', start);
    }
    if (end == std::string::npos) {
        end = url.length();
    }

    return url.substr(start, end - start);
}

// 上传配置信息到服务器
static bool uploadConfigInfo(const Config& config) {
    // 如果没有配置上传 URL，则跳过
    if (config.upload.url.empty() || config.shop.id == 0) {
        spdlog::info("Skipping config upload (upload.url or shop.id not configured)");
        return true;
    }

    try {
        // 构建 JSON 数据
        nlohmann::json j;
        j["shop_id"] = config.shop.id;  // 使用 shop_id 字段名

        nlohmann::json cameras = nlohmann::json::array();
        for (const auto& stream : config.streams) {
            nlohmann::json camera;
            camera["channel"] = stream.id;
            camera["ip"] = extractIpFromUrl(stream.url);
            cameras.push_back(camera);
        }
        j["cameras"] = cameras;

        std::string json_body = j.dump();
        spdlog::info("Uploading config info: {}", json_body);

        // 解析 URL
        std::string scheme_host_port = config.upload.url;
        std::string path = "/api/cameras/sync";

        // 发送 POST 请求
        httplib::Client cli(scheme_host_port);
        cli.set_connection_timeout(config.upload.timeout_seconds);
        cli.set_read_timeout(config.upload.timeout_seconds);
        cli.set_write_timeout(config.upload.timeout_seconds);

        spdlog::debug("Connecting to: {}, path: {}", scheme_host_port, path);
        httplib::Result res = cli.Post(path, json_body, "application/json");

        if (res) {
            if (res->status == 200 || res->status == 201) {
                spdlog::info("Config info uploaded successfully - Status: {}, Body: {}", res->status, res->body);
                return true;
            } else {
                spdlog::warn("Config info upload returned status: {}, Body: {}", res->status, res->body);
                return false;
            }
        } else {
            spdlog::error("Config info upload failed - Error: {}", httplib::to_string(res.error()));
            return false;
        }

    } catch (const std::exception& e) {
        spdlog::error("Config info upload exception: {}", e.what());
        return false;
    }
}

static spdlog::level::level_enum parseLogLevel(const std::string& level) {
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "info") return spdlog::level::info;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    if (level == "critical" || level == "off") return spdlog::level::critical;
    // Default to debug
    return spdlog::level::debug;
}

void Application::initializeLogging(const std::string& logLevel) {
    // Get executable directory for log file
    std::string logPath = "nvr.log";  // Default to current directory

    #ifdef _WIN32
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
        std::string exeDir = exePath;
        size_t pos = exeDir.find_last_of("\\/");
        if (pos != std::string::npos) {
            exeDir = exeDir.substr(0, pos);
            logPath = exeDir + "\\nvr.log";
        }
    }
    #endif

    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath, true);

    // Try to add console sink, but don't fail if it doesn't work (e.g., in service mode)
    std::vector<spdlog::sink_ptr> sinks = {file_sink};

    #ifndef _WIN32
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    #else
    // On Windows, check if we have a console
    if (GetConsoleWindow() != NULL) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    #endif

    auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
    logger->set_level(parseLogLevel(logLevel));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::set_default_logger(logger);

    spdlog::info("Logging to file: {}", logPath);
}

int Application::run(const Config& config, ApplicationState& state) {
    spdlog::info("=== NVR starting ===");
    spdlog::info("Output directory: {}", config.record.output_dir);
    spdlog::info("Temporary directory: {}", config.record.temp_dir);
    spdlog::info("Recording configuration:");
    spdlog::info("  Segment duration: {} seconds", config.record.segment_duration_seconds);
    spdlog::info("  Filename template: {}", config.record.filename_template);
    spdlog::info("  Audio recording: {}", config.record.enable_audio ? "enabled" : "disabled");
    spdlog::info("Configured {} stream(s)", config.streams.size());

    for (size_t i = 0; i < config.streams.size(); i++) {
        spdlog::info("  Stream [{}]: ID={}, URL={}", i, config.streams[i].id, config.streams[i].url);
        if (!config.streams[i].extra_params.empty()) {
            for (const auto& [key, value] : config.streams[i].extra_params) {
                spdlog::info("    {}={}", key, value);
            }
        }
    }

    spdlog::info("Auto clean configuration:");
    spdlog::info("  Enabled: {}", config.autoclean.enabled ? "yes" : "no");
    if (config.autoclean.enabled) {
        spdlog::info("  Max file age: {} hours", config.autoclean.max_age_hours);
        spdlog::info("  Max disk usage: {} GB", config.autoclean.max_disk_usage_gb);
        spdlog::info("  Check interval: {} seconds", config.autoclean.check_interval_seconds);
    }

    // Create NVR manager
    state.manager = std::make_unique<NVRManager>(config);

    // Create uploader if configured
    spdlog::info("Video upload configuration:");
    spdlog::info("  Enabled: {}", config.upload.enabled ? "yes" : "no");
    if (config.upload.enabled && !config.upload.url.empty()) {
        state.uploader = std::make_shared<VideoUploader>(config.upload);
        state.manager->setUploader(state.uploader);
        spdlog::info("  URL: {}", config.upload.url);
        spdlog::info("  Timeout: {}s", config.upload.timeout_seconds);
        spdlog::info("  Max retries: {}", config.upload.max_retries);
        spdlog::info("  Retry delay: {}s", config.upload.retry_delay_seconds);
    } else {
        if (!config.upload.enabled) {
            spdlog::info("  Status: disabled by configuration");
        } else if (config.upload.url.empty()) {
            spdlog::info("  Status: disabled (no URL configured)");
        }
    }

    // Upload shop and cameras configuration info
    spdlog::info("Shop configuration:");
    spdlog::info("  Shop ID: {}", config.shop.id);
    uploadConfigInfo(config);

    // Add all streams
    for (const auto& stream : config.streams) {
        if (!state.manager->addStreamWithConfig(stream.id, stream.url,
                                                  stream.auto_reconnect,
                                                  stream.reconnect_interval_seconds,
                                                  stream.max_reconnect_attempts,
                                                  stream.timeout_seconds)) {
            spdlog::error("Failed to add stream: {} ({})", stream.id, stream.url);
            return 1;
        }
        spdlog::info("Added stream: {} -> {}", stream.id, stream.url);
        spdlog::info("  Auto reconnect: {}", stream.auto_reconnect ? "enabled" : "disabled");
        spdlog::info("  Reconnect interval: {}s", stream.reconnect_interval_seconds);
        spdlog::info("  Max reconnect attempts: {}",
                    stream.max_reconnect_attempts == -1 ? "unlimited" : std::to_string(stream.max_reconnect_attempts));
        spdlog::info("  Stream timeout: {}s", stream.timeout_seconds);
    }

    // Start uploader
    if (state.uploader) {
        state.uploader->start();
    }

    spdlog::info("NVR is running.");

    // Main loop with shorter sleep interval for faster shutdown response
    while (state.running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Stop uploader
    if (state.uploader) {
        spdlog::info("Stopping video uploader...");
        state.uploader->stop();
    }

    spdlog::info("=== NVR stopped ===");

    return 0;
}

void Application::setupSignalHandlers(ApplicationState& state) {
    g_running_ptr = &state.running;
    g_manager_ptr = state.manager.get();

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
}
