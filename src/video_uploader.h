#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include "config_loader.h"

class VideoUploader {
public:
    VideoUploader(const UploadConfig& config);
    ~VideoUploader();

    // 启动上传线程
    void start();

    // 停止上传线程
    void stop();

    // 添加上传任务
    void enqueue(const UploadTask& task);

    // 检查是否启用
    bool isEnabled() const { return !config_.url.empty(); }

private:
    // 上传线程主函数
    void uploadLoop();

    // 执行单个文件上传
    bool uploadFile(const UploadTask& task);

    // 使用 httplib 上传文件
    bool uploadToServer(const std::string& file_path,
                       const std::string& stream_id,
                       const std::string& recording_time);

    UploadConfig config_;
    std::thread upload_thread_;
    std::queue<UploadTask> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_;
    std::atomic<bool> stopped_;
};
