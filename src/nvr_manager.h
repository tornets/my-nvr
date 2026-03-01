//
// Created by wention on 2026/1/27.
//

#ifndef NVR_NVR_MANAGER_H
#define NVR_NVR_MANAGER_H

#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <set>

#include <spdlog/spdlog.h>

#include "ipc_recorder.h"
#include "video_uploader.h"
#include "upload_progress.h"
#include "config_loader.h"

class NVRManager {
public:
    NVRManager(const Config& config);
    ~NVRManager();

    bool addStream(const std::string& stream_id, const std::string& stream_url);
    bool addStreamWithConfig(const std::string& stream_id, const std::string& stream_url,
                             bool auto_reconnect, int reconnect_interval_seconds,
                             int max_reconnect_attempts, int timeout_seconds,
                             const std::string& stream_name = "");
    bool removeStream(const std::string& stream_id);
    void stopAll();

    // 设置上传器
    void setUploader(std::shared_ptr<VideoUploader> uploader);

private:
    void cleanupLoop();
    void uploadLoop();  // 扫描新文件并上传
    bool cleanOldFiles();
    bool checkDiskUsage();
    void scanAndUploadNewFiles();
    bool cleanTempFiles();  // 清理临时目录中残留的文件

    Config m_config;

    std::unordered_map<std::string, std::unique_ptr<IPCRecorder>> m_recorders;
    std::mutex m_recorders_mutex;

    std::thread m_cleanup_thread;
    std::thread m_upload_thread;  // 上传扫描线程
    std::atomic<bool> m_running;
    std::condition_variable m_cleanup_cv;
    std::condition_variable m_upload_cv;
    std::mutex m_cleanup_mutex;
    std::mutex m_upload_mutex;

    std::shared_ptr<VideoUploader> m_uploader;
    std::shared_ptr<UploadProgressManager> m_upload_progress;  // 上传进度管理器
};

#endif //NVR_NVR_MANAGER_H
