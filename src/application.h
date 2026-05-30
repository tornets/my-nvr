#ifndef NVR_APPLICATION_H
#define NVR_APPLICATION_H

#include "config_loader.h"
#include <memory>
#include <atomic>
#include <string>

class NVRManager;
class VideoUploader;

// Application state holder
struct ApplicationState {
    std::unique_ptr<NVRManager> manager;
    std::shared_ptr<VideoUploader> uploader;
    std::atomic<bool> running{true};

    void shutdown();
};

// Core application logic
class Application {
public:
    // Initialize logging
    static void initializeLogging(const std::string& logLevel, bool consoleLog = false);

    // Main application entry point
    static int run(const Config& config, ApplicationState& state);

    // Signal handler setup
    static void setupSignalHandlers(ApplicationState& state);
};

#endif // NVR_APPLICATION_H
