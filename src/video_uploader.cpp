#include "video_uploader.h"
#include <httplib.h>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <thread>

VideoUploader::VideoUploader(const UploadConfig& config)
    : config_(config), running_(false), stopped_(false) {
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
    upload_thread_ = std::thread(&VideoUploader::uploadLoop, this);
    spdlog::info("Video uploader started, target URL: {}", config_.url);
}

void VideoUploader::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);
    queue_cv_.notify_all();

    if (upload_thread_.joinable()) {
        upload_thread_.join();
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

void VideoUploader::uploadLoop() {
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

        // 执行上传
        uploadFile(task);
    }

    // 处理剩余任务
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!task_queue_.empty()) {
        auto task = task_queue_.front();
        task_queue_.pop();
        uploadFile(task);
    }
}

bool VideoUploader::uploadFile(const UploadTask& task) {
    int retry_count = 0;

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

        // 读取文件内容
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            spdlog::error("Failed to open file: {}", file_path);
            return false;
        }

        std::string file_content;
        file.seekg(0, std::ios::end);
        file_content.resize(file.tellg());
        file.seekg(0, std::ios::beg);
        file.read(&file_content[0], file_content.size());
        file.close();

        // 解析 URL
        std::string host = config_.url;
        std::string path = "/upload";

        // 移除协议前缀
        size_t protocol_pos = host.find("://");
        if (protocol_pos != std::string::npos) {
            host = host.substr(protocol_pos + 3);
        }

        // 分离主机和路径
        size_t path_pos = host.find('/');
        if (path_pos != std::string::npos) {
            path = host.substr(path_pos);
            host = host.substr(0, path_pos);
        }

        // 提取端口（如果有的话）
        int port = 80;
        size_t port_pos = host.find(':');
        if (port_pos != std::string::npos) {
            port = std::stoi(host.substr(port_pos + 1));
            host = host.substr(0, port_pos);
        }

        // 使用 HTTPS 如果 URL 以 https 开头
        bool use_https = config_.url.find("https://") == 0;
        if (use_https && port == 80) {
            port = 443;
        }

        spdlog::debug("Connecting to: {}:{}{}", host, port, path);

        // 发送 POST 请求
        httplib::Result res;

        if (use_https) {
            // HTTPS 客户端
            httplib::SSLClient cli(host, port);
            cli.set_connection_timeout(config_.timeout_seconds);
            cli.set_read_timeout(config_.timeout_seconds);
            cli.set_write_timeout(config_.timeout_seconds);

            std::string filename = std::filesystem::path(file_path).filename().string();

            // 构建 multipart/form-data 请求
            httplib::UploadFormDataItems items = {
                {"file", file_content, filename, "video/mp4"},
                {"stream_id", stream_id},
                {"recording_time", recording_time}
            };

            res = cli.Post(path, items);
        } else {
            // HTTP 客户端
            httplib::Client cli(host, port);
            cli.set_connection_timeout(config_.timeout_seconds);
            cli.set_read_timeout(config_.timeout_seconds);
            cli.set_write_timeout(config_.timeout_seconds);

            std::string filename = std::filesystem::path(file_path).filename().string();

            // 构建 multipart/form-data 请求
            httplib::UploadFormDataItems items = {
                {"file", file_content, filename, "video/mp4"},
                {"stream_id", stream_id},
                {"recording_time", recording_time}
            };

            res = cli.Post(path, items);
        }

        if (res) {
            if (res->status == 200 || res->status == 201) {
                spdlog::debug("Upload successful: {} - Status: {}",
                             file_path, res->status);
                return true;
            } else {
                spdlog::error("Upload failed: {} - Status: {}, Body: {}",
                             file_path, res->status, res->body);
                return false;
            }
        } else {
            spdlog::error("Upload failed: {} - Error: {}", file_path, static_cast<int>(res.error()));
            return false;
        }

    } catch (const std::exception& e) {
        spdlog::error("Upload exception: {} - {}", file_path, e.what());
        return false;
    }
}
