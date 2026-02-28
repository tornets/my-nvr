#include "video_uploader.h"
#include <httplib.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <algorithm>

VideoUploader::VideoUploader(const UploadConfig& config)
    : config_(config),
      thread_count_(config.threads),
      running_(false),
      stopped_(false) {

    // 初始化进度管理器
    if (config.persist_progress) {
        progress_manager_ = std::make_shared<UploadProgressManager>(
            config.progress_file
        );
        progress_manager_->load();
    }
}

VideoUploader::~VideoUploader() {
    stop();
}

void VideoUploader::start() {
    if (!isEnabled()) {
        spdlog::info("Video upload is disabled (no URL configured)");
        return;
    }

    if (running_.load()) {
        spdlog::warn("Video uploader is already running");
        return;
    }

    running_.store(true);

    // 启动多个工作线程
    for (size_t i = 0; i < thread_count_; ++i) {
        worker_threads_.emplace_back(&VideoUploader::workerLoop, this, i);
    }

    spdlog::info("Video uploader started with {} worker thread(s), target URL: {}",
                 thread_count_, config_.url);
}

void VideoUploader::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    queue_cv_.notify_all();

    // 等待所有工作线程完成
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    worker_threads_.clear();

    // 保存进度
    if (progress_manager_) {
        progress_manager_->save();
    }

    stopped_.store(true);
    spdlog::info("Video uploader stopped");
}

void VideoUploader::enqueue(const UploadTask& task) {
    if (!isEnabled()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(task);
    }
    queue_cv_.notify_one();
    spdlog::debug("Upload task enqueued: {} (stream: {})", task.file_path, task.stream_id);
}

void VideoUploader::workerLoop(size_t thread_id) {
    spdlog::debug("Upload worker thread {} started", thread_id);

    while (running_.load()) {
        UploadTask task;

        // 从队列获取任务
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !task_queue_.empty() || !running_.load();
            });

            if (!running_.load()) {
                break;
            }

            if (task_queue_.empty()) {
                continue;
            }

            task = task_queue_.front();
            task_queue_.pop();
        }

        // 通知上传开始
        onUploadStart(task);

        // 执行上传
        bool success = uploadFile(task);
        if (success) {
            onUploadSuccess(task);
        } else {
            onUploadFailure(task, "Max retries exceeded");
        }
    }

    spdlog::debug("Upload worker thread {} stopped", thread_id);
}

bool VideoUploader::uploadFile(const UploadTask& task) {
    int retry_count = 0;
    std::string relative_path = std::filesystem::path(task.file_path).filename().string();

    while (retry_count <= config_.max_retries) {
        if (retry_count > 0) {
            spdlog::info("Retrying upload ({}/{}): {}",
                        retry_count, config_.max_retries, task.file_path);
            std::this_thread::sleep_for(
                std::chrono::seconds(config_.retry_delay_seconds));
        }

        if (uploadToServer(task.file_path, task.stream_id, task.recording_time)) {
            spdlog::info("Successfully uploaded: {}", task.file_path);
            return true;
        }

        retry_count++;

        // 更新重试计数到进度管理器
        if (progress_manager_) {
            auto record = progress_manager_->getRecord(relative_path);
            if (record) {
                record->retry_count = retry_count;
            }
        }
    }

    spdlog::error("Failed to upload after {} retries: {}",
                 config_.max_retries, task.file_path);
    return false;
}

bool VideoUploader::uploadToServer(const std::string& file_path,
                                   const std::string& stream_id,
                                   const std::string& recording_time) {
    try {
        // 检查文件是否存在
        if (!std::filesystem::exists(file_path)) {
            spdlog::error("File not found: {}", file_path);
            return false;
        }

        // 获取文件大小
        size_t file_size = std::filesystem::file_size(file_path);
        spdlog::debug("Uploading file: {} (size: {} bytes)", file_path, file_size);

        // 解析 URL
        std::string scheme_host_port = config_.url;
        std::string path = "/api/upload-batch";

        spdlog::debug("Connecting to: {}, path: {}", scheme_host_port, path);

        // 标准化文件名为 Linux 路径格式
        std::string filename = std::filesystem::path(file_path).filename().string();
        std::string file_path_normalized = file_path;
        // 将所有反斜杠替换为正斜杠（Windows 路径转 Linux 路径）
        std::replace(file_path_normalized.begin(), file_path_normalized.end(), '\\', '/');

        spdlog::debug("Upload file (normalized path): {}", file_path_normalized);

        // 二进制读取文件
        std::ifstream ifs(file_path_normalized, std::ios::binary);
        if (!ifs) {
            spdlog::error("open file failed: {}",
                          file_path);
            return false;
        }

        std::vector<char> buffer(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>()
        );

        // 构建 multipart/form-data 请求（直接传文件路径，不读取到内存）
        httplib::UploadFormDataItems items = {
            {"files", std::string(buffer.begin(), buffer.end()), filename, "video/mp4"},
        };

        // httplib::Client 自动处理 HTTP 和 HTTPS
        httplib::Client cli(scheme_host_port);
        cli.set_connection_timeout(config_.timeout_seconds);
        cli.set_read_timeout(config_.timeout_seconds);
        cli.set_write_timeout(config_.timeout_seconds);

        httplib::Result res = cli.Post(path, items);

        if (res) {
            if (res->status == 200 || res->status == 201) {
                spdlog::debug("Upload successful: {} - Status: {}, Body: {}",
                             file_path, res->status, res->body);
                return true;
            } else {
                spdlog::error("Upload failed: {} - Status: {}, Body: {}",
                             file_path, res->status, res->body);
                return false;
            }
        } else {
            spdlog::error("Upload failed: {} - Error: {}", file_path, httplib::to_string(res.error()));
            return false;
        }

    } catch (const std::exception& e) {
        spdlog::error("Upload exception: {} - {}", file_path, e.what());
        return false;
    }
}

void VideoUploader::onUploadStart(const UploadTask& task) {
    if (!progress_manager_) {
        return;
    }

    UploadRecord record;
    record.file_path = task.file_path;
    // 生成相对路径（用于去重）
    record.relative_path = std::filesystem::path(task.file_path).filename().string();
    record.stream_id = task.stream_id;
    record.recording_time = task.recording_time;
    record.status = UploadStatus::Uploading;
    record.retry_count = 0;
    record.created_at = std::chrono::system_clock::now();
    record.updated_at = record.created_at;

    try {
        record.file_size = std::filesystem::file_size(task.file_path);
    } catch (...) {
        record.file_size = 0;
    }

    progress_manager_->addRecord(record);
    spdlog::debug("Upload started: {}", record.relative_path);
}

void VideoUploader::onUploadSuccess(const UploadTask& task) {
    if (!progress_manager_) {
        return;
    }

    std::string relative_path = std::filesystem::path(task.file_path).filename().string();
    progress_manager_->updateRecord(relative_path, UploadStatus::Success);
    spdlog::debug("Upload success: {}", relative_path);
}

void VideoUploader::onUploadFailure(const UploadTask& task, const std::string& error) {
    if (!progress_manager_) {
        return;
    }

    std::string relative_path = std::filesystem::path(task.file_path).filename().string();
    progress_manager_->updateRecord(relative_path, UploadStatus::Failed, error);
    spdlog::debug("Upload failed: {} - {}", relative_path, error);
}
