#include "log.h"

#include "video_uploader.h"
#include <httplib.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <thread>
#include <algorithm>

using namespace std::chrono_literals;


thread_local int VideoUploader::lastError_ = 0;
thread_local std::string VideoUploader::lastErrorString_ = "OK";

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
        LOG_INFO("Video upload is disabled (no URL configured)");
        return;
    }

    if (running_.load()) {
        LOG_WARN("Video uploader is already running");
        return;
    }

    running_.store(true);

    // 启动多个工作线程
    for (size_t i = 0; i < thread_count_; ++i) {
        worker_threads_.emplace_back(&VideoUploader::workerLoop, this, i);
    }

    LOG_INFO("Video uploader started with {} worker thread(s), target URL: {}",
                 thread_count_, config_.url);

    // restore upload tasks
    auto pending = progress_manager_->getPendingRecords();
    for (auto& item : pending) {
        UploadTask task = {
                item.file_path,
                item.stream_id,
                item.recording_time
        };

        upload(task);
    }
    // 恢复之前失败的上传任务（将在 uploader 启动后重新入队）
    LOG_INFO("Loaded {} upload records from progress file",
             pending.size());
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
    LOG_INFO("Video uploader stopped");
}

void VideoUploader::upload(const UploadTask& task) {
    if (!isEnabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!progress_manager_) {
        return;
    }

    UploadRecord record;
    record.file_path = task.file_path;
    // 生成相对路径（用于去重）
    record.relative_path = std::filesystem::path(task.file_path).filename().string();
    record.stream_id = task.stream_id;
    record.recording_time = task.recording_time;
    record.status = UploadStatus::Pending;
    record.retry_count = 0;
    record.created_at = std::chrono::system_clock::now();
    record.updated_at = record.created_at;

    try {
        record.file_size = std::filesystem::file_size(task.file_path);
    } catch (...) {
        record.file_size = 0;
    }

    progress_manager_->addRecord(record);

    queue_cv_.notify_one();
    LOG_INFO("Upload task scheduled: {} (stream: {})", record.relative_path, task.stream_id);
}

void VideoUploader::workerLoop(size_t thread_id) {
    LOG_INFO("Upload worker thread {} started", thread_id);

    while (running_.load()) {
        UploadTask task;
        UploadRecord record;

        // 从队列获取任务
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, 1000ms, [this] {
                return !running_.load();
            });

            if (!running_.load()) {
                break;
            }

            auto pending = progress_manager_->getPendingRecords();
            if (pending.empty())
                continue;

            bool found = false;
            for (auto& item : pending) {
                record = item;
                if (item.status == UploadStatus::Pending) {
                    found = true;
                    break;
                } else if (item.status == UploadStatus::Failed) {
                    auto now = std::chrono::system_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - record.updated_at);
                    if (duration.count() >= config_.retry_delay_seconds) {
                        found = true;
                        break;
                    }
                }
            }

            if (!found)
                continue;

            task = {
                record.file_path,
                record.stream_id,
                record.recording_time,
            };

            // fast fail
            if (!std::filesystem::exists(task.file_path)) {
                LOG_WARN("upload task error: file not found - {}", task.file_path);
                continue;
            }
        }

        // 通知上传开始
        LOG_INFO("starting upload task: thread={}, file={}", thread_id, task.file_path);
        onUploadStart(task);

        // 执行上传
        bool success = uploadFile(task);
        if (success) {
            onUploadSuccess(task);
        } else {
            onUploadFailure(task, lastErrorString_);
        }
    }

    LOG_DEBUG("Upload worker thread {} stopped", thread_id);
}

bool VideoUploader::uploadFile(const UploadTask& task) {
    std::string relative_path = std::filesystem::path(task.file_path).filename().string();

    auto record = progress_manager_->getRecord(relative_path);
    int retry_count = record ? record->retry_count : 0;

    if (config_.max_retries < 0 || retry_count <= config_.max_retries) {
        if (uploadToServer(task.file_path, task.stream_id, task.recording_time)) {
            LOG_INFO("Successfully uploaded: {}", task.file_path);
            return true;
        }

        retry_count++;

        // 更新重试计数到进度管理器
        if (progress_manager_) {
            record = progress_manager_->getRecord(relative_path);
            if (record) {
                record->retry_count = retry_count;
                record->updated_at = std::chrono::system_clock::now();
            }
        }
    }

    return false;
}

bool VideoUploader::uploadToServer(const std::string& file_path,
                                   const std::string& stream_id,
                                   const std::string& recording_time) {
    try {
        // 检查文件是否存在
        if (!std::filesystem::exists(file_path)) {
            lastError_ = -1;
            lastErrorString_ = "file not found: " + file_path;
            LOG_ERROR("File not found: {}", file_path);
            return false;
        }

        // 获取文件大小
        size_t file_size = std::filesystem::file_size(file_path);
        LOG_DEBUG("Uploading file: {} (size: {} bytes)", file_path, file_size);

        // 解析 URL
        std::string scheme_host_port = config_.url;
        std::string path = "/api/upload-batch";

        LOG_DEBUG("Connecting to: {}, path: {}", scheme_host_port, path);

        // 标准化文件名为 Linux 路径格式
        std::string filename = std::filesystem::path(file_path).filename().string();
        std::string file_path_normalized = file_path;
        // 将所有反斜杠替换为正斜杠（Windows 路径转 Linux 路径）
        std::replace(file_path_normalized.begin(), file_path_normalized.end(), '\\', '/');

        LOG_DEBUG("Upload file (normalized path): {}", file_path_normalized);

        // 二进制读取文件
        std::ifstream ifs(file_path_normalized, std::ios::binary);
        if (!ifs) {
            lastError_ = -1;
            lastErrorString_ = "open file failed: " + file_path;
            LOG_ERROR("open file failed: {}",
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
                lastError_ = res->status;
                lastErrorString_ = res->body;
                LOG_DEBUG("Upload successful: {} - Status: {}, Body: {}",
                             file_path, res->status, res->body);
                return true;
            } else {
                lastError_ = res->status;
                lastErrorString_ = res->body;
                LOG_ERROR("Upload failed: {} - Status: {}, Body: {}",
                             file_path, res->status, res->body);
                return false;
            }
        } else {
            lastError_ = static_cast<int>(res.error());
            lastErrorString_ = httplib::to_string(res.error());
            LOG_ERROR("Upload failed: {} - Error: {}", file_path, httplib::to_string(res.error()));
            return false;
        }

    } catch (const std::exception& e) {
        lastError_ = -1;
        lastErrorString_ = e.what();
        LOG_ERROR("Upload exception: {} - {}", file_path, e.what());
        return false;
    }
}

void VideoUploader::onUploadStart(const UploadTask& task) {
    if (!progress_manager_) {
        return;
    }

    std::string relative_path = std::filesystem::path(task.file_path).filename().string();
    progress_manager_->updateRecord(relative_path, UploadStatus::Uploading);
    LOG_INFO("Upload started: {}", relative_path);
}

void VideoUploader::onUploadSuccess(const UploadTask& task) {
    if (!progress_manager_) {
        return;
    }

    std::string relative_path = std::filesystem::path(task.file_path).filename().string();
    progress_manager_->updateRecord(relative_path, UploadStatus::Success);
    LOG_INFO("Upload success: {}", relative_path);
}

void VideoUploader::onUploadFailure(const UploadTask& task, const std::string& error) {
    if (!progress_manager_) {
        return;
    }

    std::string relative_path = std::filesystem::path(task.file_path).filename().string();
    progress_manager_->updateRecord(relative_path, UploadStatus::Failed, error);
    LOG_ERROR("Upload failed: {} - {}", relative_path, error);
}
