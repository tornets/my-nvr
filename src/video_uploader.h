#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <vector>
#include "config_loader.h"
#include "upload_progress.h"

class VideoUploader {
public:
    VideoUploader(const UploadConfig& config);
    ~VideoUploader();

    // 启动上传线程
    void start();

    // 停止上传线程
    void stop();

    // 添加上传任务
    void upload(const UploadTask& task);

    // 检查是否启用
    bool isEnabled() const { return !config_.url.empty(); }

    std::shared_ptr<UploadProgressManager> progress() const { return progress_manager_; }

private:
    // 工作线程主函数
    void workerLoop(size_t thread_id);

    // 执行单个文件上传
    bool uploadFile(const UploadTask& task);

    // 使用 httplib 上传文件
    bool uploadToServer(const std::string& file_path,
                       const std::string& stream_id,
                       const std::string& recording_time);

    // 进度回调
    void onUploadStart(const UploadTask& task);
    void onUploadSuccess(const UploadTask& task);
    void onUploadFailure(const UploadTask& task, const std::string& error);

    UploadConfig config_;

    // 线程池
    std::vector<std::thread> worker_threads_;
    size_t thread_count_;

    // 任务队列
    std::queue<UploadTask> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // 进度管理
    std::shared_ptr<UploadProgressManager> progress_manager_;

    // 同步控制
    std::atomic<bool> running_;
    std::atomic<bool> stopped_;
};
