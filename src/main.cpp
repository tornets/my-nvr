extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <iostream>
#include <csignal>
#include <memory>
#include <vector>

#ifdef _WIN32
#include "win32_service.h"
#endif
#include "application.h"
#include "nvr_manager.h"
#include "config_loader.h"
#include "video_uploader.h"
#include "cmdline_parser.h"
#include "version.h"
#include <spdlog/spdlog.h>

// Forward declarations
int handleServiceInstall(const std::vector<std::string>& args);
int handleServiceUninstall();
int handleServiceStart();
int handleServiceStop();
int handleServiceRestart();
int runServiceMode(int argc, char* argv[]);
int runConsoleMode(int argc, char* argv[]);

int main(int argc, char* argv[]) {
    // Check for --version flag
    if (argc > 1 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        std::cout << "version: " << NVR::getDetailedVersionString() << std::endl;
        std::cout << "build at: " << NVR::getBuildDateString() << std::endl;
        std::cout << "build type: " << NVR::getBuildTypeString() << std::endl;
        return 0;
    }

    // Check if first argument is a subcommand
    if (argc > 1) {
        std::string arg1 = argv[1];

        // Service management subcommands
        if (arg1 == "service") {
            if (argc < 3) {
                std::cerr << "Usage: nvr service <install|uninstall|start|stop|restart> [options]" << std::endl;
                return 1;
            }

            std::string cmd = argv[2];
            std::vector<std::string> extraArgs;
            for (int i = 3; i < argc; i++) {
                extraArgs.push_back(argv[i]);
            }

            if (cmd == "install") {
                return handleServiceInstall(extraArgs);
            } else if (cmd == "uninstall") {
                return handleServiceUninstall();
            } else if (cmd == "start") {
                return handleServiceStart();
            } else if (cmd == "stop") {
                return handleServiceStop();
            } else if (cmd == "restart") {
                return handleServiceRestart();
            } else {
                std::cerr << "Unknown service command: " << cmd << std::endl;
                std::cerr << "Valid commands: install, uninstall, start, stop, restart" << std::endl;
                return 1;
            }
        }
        // Service mode entry (called by SCM)
        else if (arg1 == "svr") {
            return runServiceMode(argc, argv);
        }
    }

    // Default: run in console mode
    return runConsoleMode(argc, argv);
}

int handleServiceInstall(const std::vector<std::string>& args) {
#ifdef _WIN32
    Win32Service service("nvr", "NVR Video Recorder Service");

    // Build extra args vector, skip "--" separator
    std::vector<std::string> extraArgs;
    for (const auto& arg : args) {
        if (arg != "--") {
            extraArgs.push_back(arg);
        }
    }

    if (service.install("", extraArgs)) {
        std::cout << "Service installed successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to install service: " << service.getLastError() << std::endl;
        return 1;
    }
#else
    std::cerr << "Service management is only supported on Windows" << std::endl;
    return 1;
#endif
}

int handleServiceUninstall() {
#ifdef _WIN32
    Win32Service service("nvr", "NVR Video Recorder Service");
    if (service.uninstall()) {
        std::cout << "Service uninstalled successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to uninstall service: " << service.getLastError() << std::endl;
        return 1;
    }
#else
    std::cerr << "Service management is only supported on Windows" << std::endl;
    return 1;
#endif
}

int handleServiceStart() {
#ifdef _WIN32
    Win32Service service("nvr", "NVR Video Recorder Service");
    if (service.start()) {
        std::cout << "Service started successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to start service: " << service.getLastError() << std::endl;
        return 1;
    }
#else
    std::cerr << "Service management is only supported on Windows" << std::endl;
    return 1;
#endif
}

int handleServiceStop() {
#ifdef _WIN32
    Win32Service service("nvr", "NVR Video Recorder Service");
    if (service.stop()) {
        std::cout << "Service stopped successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to stop service: " << service.getLastError() << std::endl;
        return 1;
    }
#else
    std::cerr << "Service management is only supported on Windows" << std::endl;
    return 1;
#endif
}

int handleServiceRestart() {
#ifdef _WIN32
    Win32Service service("nvr", "NVR Video Recorder Service");
    if (service.restart()) {
        std::cout << "Service restarted successfully" << std::endl;
        return 0;
    } else {
        std::cerr << "Failed to restart service: " << service.getLastError() << std::endl;
        return 1;
    }
#else
    std::cerr << "Service management is only supported on Windows" << std::endl;
    return 1;
#endif
}

int runServiceMode(int argc, char* argv[]) {
#ifdef _WIN32
    // Load config from command line (skip "svr" argument)
    std::vector<char*> svcArgv;
    svcArgv.push_back(argv[0]);
    for (int i = 2; i < argc; i++) {
        svcArgv.push_back(argv[i]);
    }

    // Initialize logging early to capture any errors
    Application::initializeLogging("debug");

    try {
        spdlog::info("{}", NVR::getDetailedVersionString());
        spdlog::info("Service mode starting with {} arguments", svcArgv.size());
        for (size_t i = 0; i < svcArgv.size(); i++) {
            spdlog::info("  Arg[{}]: {}", i, svcArgv[i]);
        }

        Config config = parseCommandLine(static_cast<int>(svcArgv.size()), svcArgv.data());

        if (config.streams.empty()) {
            spdlog::error("No streams configured for service");
            std::cerr << "Error: No streams configured for service." << std::endl;
            return 1;
        }

        spdlog::info("Config loaded successfully, {} stream(s) configured", config.streams.size());

        Win32Service service("NVRService", "NVR Video Recorder Service");
        ApplicationState state;

        service.setServiceStopCallback([&state]() {
            spdlog::info("Service stop callback triggered");
            state.shutdown();
        });

        spdlog::info("Starting service dispatcher...");
        service.runAsService([&]() {
            spdlog::info("Service main function started");
            Application::setupSignalHandlers(state);
            Application::run(config, state);
            spdlog::info("Service main function completed");
        });

        spdlog::info("Service mode completed successfully");
        return 0;
    } catch (const std::exception& e) {
        spdlog::critical("Error in service mode: {}", e.what());
        std::cerr << "Error in service mode: " << e.what() << std::endl;
        return 1;
    }
#else
    std::cerr << "Service mode is only supported on Windows" << std::endl;
    return 1;
#endif
}

int runConsoleMode(int argc, char* argv[]) {
    try {
        Config config = parseCommandLine(argc, argv);

        if (config.streams.empty()) {
            std::cerr << "Error: No streams configured." << std::endl;
            std::cerr << "Please provide stream arguments or config file." << std::endl;
            std::cerr << std::endl;
            std::cerr << "Usage:" << std::endl;
            std::cerr << "  " << argv[0] << " [options] stream1,url=rtsp://host/path" << std::endl;
            std::cerr << std::endl;
            std::cerr << "Options:" << std::endl;
            std::cerr << "  --work-dir DIR          Working directory (change before starting)" << std::endl;
            std::cerr << "  -c, --config FILE        Configuration file path (YAML)" << std::endl;
            std::cerr << "  -l, --log-level LEVEL    Log level (trace/debug/info/warn/error/critical)" << std::endl;
            std::cerr << std::endl;
            std::cerr << "Service Management:" << std::endl;
            std::cerr << "  " << argv[0] << " service install -- --config C:\\nvr\\config.yaml" << std::endl;
            std::cerr << "  " << argv[0] << " service start" << std::endl;
            std::cerr << "  " << argv[0] << " service stop" << std::endl;
            std::cerr << "  " << argv[0] << " service uninstall" << std::endl;
            return 1;
        }

        Application::initializeLogging(config.log_level);

        spdlog::info("Version: {}", NVR::getDetailedVersionString());
        spdlog::info("Built: {}", NVR::getBuildDateString());
        spdlog::info("Build type: {}", NVR::getBuildTypeString());

        ApplicationState state;
        Application::setupSignalHandlers(state);

        spdlog::info("NVR is running in console mode. Press Ctrl+C to stop.");

        return Application::run(config, state);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
