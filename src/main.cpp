extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <iostream>
#include <csignal>
#include <memory>

#include "nvr_manager.h"
#include "config_loader.h"
#include "video_uploader.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <argparse/argparse.hpp>

std::unique_ptr<NVRManager> g_manager;
std::atomic<bool> g_running(true);

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        spdlog::info("Received signal, shutting down...");
        g_running = false;
        if (g_manager) {
            g_manager->stopAll();
        }
    }
}

// 将字符串日志级别转换为 spdlog::level
spdlog::level::level_enum parseLogLevel(const std::string& level) {
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "info") return spdlog::level::info;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    if (level == "critical" || level == "off") return spdlog::level::critical;
    // 默认使用 debug
    return spdlog::level::debug;
}

int main(int argc, char* argv[]) {
    // 创建 argparse 解析器
    argparse::ArgumentParser program("nvr", "1.0");

    // 添加配置文件参数
    program.add_argument("-c", "--config")
        .help("Configuration file path (YAML)")
        .default_value(std::string(""))
        .nargs(1);

    // 添加日志级别参数
    program.add_argument("-l", "--log-level")
        .help("Log level: trace, debug, info, warn, error, critical")
        .default_value(std::string(""))
        .nargs(1);

    // 添加上传配置参数
    program.add_argument("--upload-url")
        .help("Upload server URL (enables upload)")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--upload-timeout")
        .help("Upload timeout in seconds")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--upload-retries")
        .help("Upload max retries")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    // 添加录制配置参数
    program.add_argument("--record-output-dir")
        .help("Recording output directory")
        .default_value(std::string(""))
        .nargs(1);

    program.add_argument("--record-segment-duration")
        .help("Recording segment duration (seconds)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    // 添加自动清理配置参数
    program.add_argument("--autoclean-max-age")
        .help("Maximum age of files to keep (hours)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--autoclean-max-disk")
        .help("Maximum disk usage (GB)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    program.add_argument("--autoclean-interval")
        .help("Cleanup check interval (seconds)")
        .default_value(0)
        .nargs(1)
        .action([](const std::string& value) { return std::stoi(value); });

    // 添加剩余参数作为流配置
    program.add_argument("streams")
        .help("Stream configurations in format: streamid,url=rtsp://host/path[,key=value]*")
        .remaining()
        .default_value(std::vector<std::string>{})
        .nargs(0, 100);  // 0-100个流

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    // 1. 加载配置
    Config config = Config::getDefault();
    std::string config_path;

    try {
        config_path = program.get<std::string>("--config");
    } catch (...) {
        config_path = "";
    }

    if (!config_path.empty()) {
        // 使用指定的配置文件
        if (auto loaded = Config::fromYaml(config_path)) {
            config = *loaded;
            spdlog::info("Loaded config from: {}", config_path);
        } else {
            std::cerr << "Warning: Failed to load config file: " << config_path << std::endl;
            std::cerr << "Using default configuration" << std::endl;
        }
    } else {
        // 搜索默认配置文件
        if (auto found = Config::findConfigFile()) {
            if (auto loaded = Config::fromYaml(*found)) {
                config = *loaded;
                spdlog::info("Auto-loaded config from: {}", *found);
            }
        }
    }

    // 1.5. 检查命令行指定的日志级别
    try {
        std::string log_level = program.get<std::string>("--log-level");
        if (!log_level.empty()) {
            config.log_level = log_level;
            std::cout << "Log level set from command line: " << log_level << std::endl;
        }
    } catch (...) {
        // 没有指定日志级别，使用配置文件中的值
    }

    // 1.6. 检查命令行指定的上传配置
    try {
        std::string upload_url = program.get<std::string>("--upload-url");
        if (!upload_url.empty()) {
            config.upload.url = upload_url;
            std::cout << "Upload URL set from command line: " << upload_url << std::endl;
        }
    } catch (...) {
        // 没有指定上传 URL
    }

    try {
        int upload_timeout = program.get<int>("--upload-timeout");
        if (upload_timeout > 0) {
            config.upload.timeout_seconds = upload_timeout;
            std::cout << "Upload timeout set from command line: " << upload_timeout << "s" << std::endl;
        }
    } catch (...) {
        // 没有指定上传超时
    }

    try {
        int upload_retries = program.get<int>("--upload-retries");
        if (upload_retries > 0) {
            config.upload.max_retries = upload_retries;
            std::cout << "Upload max retries set from command line: " << upload_retries << std::endl;
        }
    } catch (...) {
        // 没有指定重试次数
    }

    // 1.6.5. 检查命令行指定的录制配置
    try {
        std::string record_dir = program.get<std::string>("--record-output-dir");
        if (!record_dir.empty()) {
            config.record.output_dir = record_dir;
            std::cout << "Record output directory set from command line: " << record_dir << std::endl;
        }
    } catch (...) {
        // 没有指定录制目录
    }

    try {
        int segment_duration = program.get<int>("--record-segment-duration");
        if (segment_duration > 0) {
            config.record.segment_duration_seconds = segment_duration;
            std::cout << "Record segment duration set from command line: " << segment_duration << " seconds" << std::endl;
        }
    } catch (...) {
        // 没有指定分段时长
    }

    // 1.7. 检查命令行指定的自动清理配置
    try {
        int max_age = program.get<int>("--autoclean-max-age");
        if (max_age > 0) {
            config.autoclean.max_age_hours = max_age;
            std::cout << "Auto clean max age set from command line: " << max_age << " hours" << std::endl;
        }
    } catch (...) {
        // 没有指定最大文件年龄
    }

    try {
        int max_disk = program.get<int>("--autoclean-max-disk");
        if (max_disk > 0) {
            config.autoclean.max_disk_usage_gb = max_disk;
            std::cout << "Auto clean max disk usage set from command line: " << max_disk << " GB" << std::endl;
        }
    } catch (...) {
        // 没有指定最大磁盘使用量
    }

    try {
        int clean_interval = program.get<int>("--autoclean-interval");
        if (clean_interval > 0) {
            config.autoclean.check_interval_seconds = clean_interval;
            std::cout << "Auto clean check interval set from command line: " << clean_interval << " seconds" << std::endl;
        }
    } catch (...) {
        // 没有指定清理间隔
    }

    // 2. 解析位置参数（流配置）
    try {
        auto stream_args = program.get<std::vector<std::string>>("streams");

        for (const auto& arg : stream_args) {
            if (auto stream = Config::parseStreamArgument(arg)) {
                config.mergeStream(*stream);
                spdlog::info("Merged stream: {} -> {}", stream->id, stream->url);
            } else {
                std::cerr << "Warning: Failed to parse stream argument: " << arg << std::endl;
            }
        }
    } catch (...) {
        // 没有流参数
    }

    // 3. 验证配置
    if (config.streams.empty()) {
        std::cerr << "Error: No streams configured." << std::endl;
        std::cerr << "Please provide stream arguments or config file." << std::endl;
        std::cerr << std::endl;
        std::cerr << "Usage:" << std::endl;
        std::cerr << "  " << argv[0] << " [options] stream1,url=rtsp://host/path [stream2,url=rtsp://host2/path]" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Examples:" << std::endl;
        std::cerr << "  " << argv[0] << " --config config.yaml" << std::endl;
        std::cerr << "  " << argv[0] << " camera1,url=rtsp://192.168.1.100/stream" << std::endl;
        std::cerr << "  " << argv[0] << " -c config.yaml camera1,url=rtsp://new-url/stream" << std::endl;
        std::cerr << "  " << argv[0] << " --log-level info camera1,url=rtsp://192.168.1.100/stream" << std::endl;
        std::cerr << "  " << argv[0] << " --upload-url https://server.com/upload camera1,url=rtsp://host/stream" << std::endl;
        std::cerr << "  " << argv[0] << " --autoclean-max-age 24 --autoclean-max-disk 50 camera1,url=rtsp://host/stream" << std::endl;
        std::cerr << std::endl;
        std::cerr << program;
        return 1;
    }

    // 4. 初始化日志
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("nvr.log", true);

    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
    logger->set_level(parseLogLevel(config.log_level));
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::set_default_logger(logger);

    spdlog::info("=== NVR starting ===");
    spdlog::info("Output directory: {}", config.record.output_dir);
    spdlog::info("Recording configuration:");
    spdlog::info("  Segment duration: {} seconds", config.record.segment_duration_seconds);
    spdlog::info("Configured {} stream(s)", config.streams.size());

    for (size_t i = 0; i < config.streams.size(); i++) {
        spdlog::info("  Stream [{}]: ID={}, URL={}", i, config.streams[i].id, config.streams[i].url);
        if (!config.streams[i].extra_params.empty()) {
            for (const auto& [key, value] : config.streams[i].extra_params) {
                spdlog::info("    {}={}", key, value);
            }
        }
    }

    // 显示自动清理配置
    spdlog::info("Auto clean configuration:");
    spdlog::info("  Enabled: {}", config.autoclean.enabled ? "yes" : "no");
    if (config.autoclean.enabled) {
        spdlog::info("  Max file age: {} hours", config.autoclean.max_age_hours);
        spdlog::info("  Max disk usage: {} GB", config.autoclean.max_disk_usage_gb);
        spdlog::info("  Check interval: {} seconds", config.autoclean.check_interval_seconds);
    }

    // 5. 创建NVR管理器（直接传入 Config 对象）
    g_manager = std::make_unique<NVRManager>(config);

    // 5.5. 创建并启动上传器（如果配置了）
    std::shared_ptr<VideoUploader> uploader;
    spdlog::info("Video upload configuration:");
    spdlog::info("  Enabled: {}", config.upload.enabled ? "yes" : "no");
    if (config.upload.enabled && !config.upload.url.empty()) {
        uploader = std::make_shared<VideoUploader>(config.upload);
        g_manager->setUploader(uploader);
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

    // 6. 添加所有流
    for (const auto& stream : config.streams) {
        if (!g_manager->addStream(stream.id, stream.url)) {
            spdlog::error("Failed to add stream: {} ({})", stream.id, stream.url);
            return 1;
        }
        spdlog::info("Added stream: {} -> {}", stream.id, stream.url);
    }

    // 6.5. 启动上传器
    if (uploader) {
        uploader->start();
    }

    // 7. 注册信号处理
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    spdlog::info("NVR is running. Press Ctrl+C to stop.");

    // 8. 主循环
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // 9. 停止上传器
    if (uploader) {
        spdlog::info("Stopping video uploader...");
        uploader->stop();
    }

    spdlog::info("=== NVR stopped ===");

    return 0;
}
