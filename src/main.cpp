extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <iostream>
#include <csignal>
#include <memory>

#include "nvr_manager.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

std::unique_ptr<NVRManager> g_manager;
std::atomic<bool> g_running(true);

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        std::cout << "\nReceived signal, shutting down..." << std::endl;
        g_running = false;
        if (g_manager) {
            g_manager->stopAll();
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <rtsp_url> [output_dir]" << std::endl;
        std::cerr << "Example: " << argv[0] << " rtsp://192.168.1.100:554/stream ./recordings" << std::endl;
        return -1;
    }

    const std::string stream_url = argv[1];
    std::string output_dir = argc >= 3 ? argv[2] : "./recordings";

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("nvr.log", true);

    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};
    auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
    spdlog::set_default_logger(logger);

    spdlog::info("=== NVR starting ===");
    spdlog::info("Stream URL: {}", stream_url);
    spdlog::info("Output directory: {}", output_dir);

    CleanerConfig cleaner_config;
    cleaner_config.max_age_hours = 168;
    cleaner_config.max_disk_usage_gb = 100;
    cleaner_config.check_interval_seconds = 3;

    g_manager = std::make_unique<NVRManager>(output_dir, cleaner_config);

    if (!g_manager->addStream("camera1", stream_url)) {
        spdlog::error("Failed to add stream");
        return -1;
    }

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    spdlog::info("NVR is running. Press Ctrl+C to stop.");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    spdlog::info("=== NVR stopped ===");

    return 0;
}
