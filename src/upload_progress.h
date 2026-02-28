#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <memory>

enum class UploadStatus {
    Pending,        // 等待上传
    Uploading,      // 正在上传
    Success,        // 上传成功
    Failed          // 上传失败
};

struct UploadRecord {
    std::string file_path;           // 文件绝对路径
    std::string relative_path;       // 相对路径（用于去重）
    std::string stream_id;
    std::string recording_time;

    UploadStatus status;
    int retry_count;
    std::string last_error;

    std::chrono::system_clock::time_point created_at;
    std::chrono::system_clock::time_point updated_at;

    uint64_t file_size;
};

class UploadProgressManager {
public:
    explicit UploadProgressManager(const std::string& progress_file);
    ~UploadProgressManager();

    // 禁止拷贝和赋值
    UploadProgressManager(const UploadProgressManager&) = delete;
    UploadProgressManager& operator=(const UploadProgressManager&) = delete;

    // 加载和保存进度
    bool load();
    bool save();

    // 记录管理
    bool addRecord(const UploadRecord& record);
    bool updateRecord(const std::string& relative_path,
                     UploadStatus status,
                     const std::string& error = "");
    UploadRecord* getRecord(const std::string& relative_path);

    // 状态查询
    bool isUploaded(const std::string& relative_path) const;
    bool isPendingOrUploading(const std::string& relative_path) const;

    // 获取待处理记录
    std::vector<UploadRecord> getPendingRecords() const;

    // 清理旧记录
    void clearSuccessfulRecords(int older_than_hours = 24);

    // 线程安全
    mutable std::mutex records_mutex_;

private:
    std::string progress_file_;
    std::vector<UploadRecord> records_;
    std::unordered_map<std::string, size_t> path_to_index_;

    // 自动保存限流（避免频繁磁盘写入）
    std::chrono::system_clock::time_point last_save_;
    const std::chrono::seconds save_interval_{30};  // 最多 30 秒保存一次
    bool shouldSave() const;
};
