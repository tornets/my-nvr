#include "log.h"
#include "upload_progress.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <iostream>

using json = nlohmann::json;

// JSON 序列化枚举
NLOHMANN_JSON_SERIALIZE_ENUM(UploadStatus, {
    {UploadStatus::Pending, "pending"},
    {UploadStatus::Uploading, "uploading"},
    {UploadStatus::Success, "success"},
    {UploadStatus::Failed, "failed"}
})

// 时间点转字符串
std::string time_to_string(const std::chrono::system_clock::time_point& tp) {
    time_t t = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// 字符串转时间点
std::chrono::system_clock::time_point string_to_time(const std::string& s) {
    std::tm tm = {};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

// UploadRecord JSON 序列化
void to_json(json& j, const UploadRecord& record) {
    // 使用 nlohmann::json 的 string_t 构造函数来正确处理 UTF-8
    j["file_path"] = std::string(record.file_path);
    j["relative_path"] = std::string(record.relative_path);
    j["stream_id"] = std::string(record.stream_id);
    j["recording_time"] = std::string(record.recording_time);
    j["status"] = record.status;
    j["retry_count"] = record.retry_count;
    j["last_error"] = std::string(record.last_error);
    j["created_at"] = time_to_string(record.created_at);
    j["updated_at"] = time_to_string(record.updated_at);
    j["file_size"] = record.file_size;
}

void from_json(const json& j, UploadRecord& record) {
    record.file_path = j.value("file_path", "");
    record.relative_path = j.value("relative_path", "");
    record.stream_id = j.value("stream_id", "");
    record.recording_time = j.value("recording_time", "");
    record.status = j.value("status", UploadStatus::Pending);
    record.retry_count = j.value("retry_count", 0);
    record.last_error = j.value("last_error", "");

    if (j.contains("created_at")) {
        record.created_at = string_to_time(j["created_at"].get<std::string>());
    } else {
        record.created_at = std::chrono::system_clock::now();
    }

    if (j.contains("updated_at")) {
        record.updated_at = string_to_time(j["updated_at"].get<std::string>());
    } else {
        record.updated_at = std::chrono::system_clock::now();
    }

    record.file_size = j.value("file_size", uint64_t{0});
}

// UploadProgressManager 实现
UploadProgressManager::UploadProgressManager(const std::string& progress_file)
    : progress_file_(progress_file),
      last_save_(std::chrono::system_clock::now() - std::chrono::hours(1)) {
}

UploadProgressManager::~UploadProgressManager() {
    // 析构时自动保存
    save();
}

bool UploadProgressManager::load() {
    try {
        std::ifstream ifs(progress_file_);
        if (!ifs.is_open()) {
            LOG_INFO("No existing progress file found, starting fresh");
            return true;  // 文件不存在是正常情况
        }

        json j;
        ifs >> j;

        std::lock_guard<std::recursive_mutex> lock(records_mutex_);

        records_.clear();
        if (j.contains("records")) {
            for (auto& item : j["records"].get<std::vector<UploadRecord>>()) {
                // 过滤不存在/已删除的文件
                if (std::filesystem::exists(item.file_path)) {
                    records_.push_back(item);
                }
            }
        }

        // 重建索引
        path_to_index_.clear();
        for (size_t i = 0; i < records_.size(); ++i) {
            path_to_index_[records_[i].relative_path] = i;

            // reset status
            if (records_[i].status == UploadStatus::Uploading) {
                records_[i].status = UploadStatus::Pending;
            }
        }

        LOG_INFO("Loaded {} upload records from {}", records_.size(), progress_file_);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load progress file: {}", e.what());

        // 尝试备份损坏的文件
        try {
            std::string backup_path = progress_file_ + ".corrupted";
            std::filesystem::rename(progress_file_, backup_path);
            LOG_INFO("Corrupted progress file backed up to {}", backup_path);
        } catch (...) {
            // 备份失败，忽略
        }

        return false;
    }
}

bool UploadProgressManager::save() {
    try {
        json j;
        j["version"] = 1;
        j["last_updated"] = time_to_string(std::chrono::system_clock::now());

        int running_cnt = 0, failed_cnt = 0, pending_cnt = 0;
        for (const auto& record : records_) {
            if (record.status == UploadStatus::Pending) {
                pending_cnt++;
            } else if (record.status == UploadStatus::Failed) {
                failed_cnt++;
            } else if (record.status == UploadStatus::Uploading) {
                running_cnt++;
            }
        }

        j["total_cnt"] = records_.size();
        j["failed_cnt"] = failed_cnt;
        j["running_cnt"] = running_cnt;
        j["pending_cnt"] = pending_cnt;

        {
            std::lock_guard<std::recursive_mutex> lock(records_mutex_);
            j["records"] = records_;
        }

        // 原子写入（先写临时文件，再重命名）
        std::string temp_file = progress_file_ + ".tmp";
        std::ofstream ofs(temp_file, std::ios::binary);
        if (!ofs.is_open()) {
            LOG_ERROR("Failed to open temp file for writing: {}", temp_file);
            return false;
        }

        // 使用 ensure_ascii=false 来正确处理非 ASCII 字符
        // 并设置错误处理模式为 ignore 以跳过无效字符
        try {
            ofs << j.dump(2, ' ', false, json::error_handler_t::ignore);
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to serialize JSON: {}", e.what());
            return false;
        }
        ofs.close();

        // 原子重命名
        std::error_code ec;
        std::filesystem::rename(temp_file, progress_file_, ec);
        if (ec) {
            LOG_ERROR("Failed to rename progress file: {}", ec.message());
            return false;
        }

        last_save_ = std::chrono::system_clock::now();
        LOG_DEBUG("Saved {} upload records to {}", records_.size(), progress_file_);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save progress file: {}", e.what());
        return false;
    }
}

bool UploadProgressManager::shouldSave() const {
    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_save_).count();
    return elapsed >= save_interval_.count();
}

bool UploadProgressManager::addRecord(const UploadRecord& record) {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    // 检查是否已存在
    auto it = path_to_index_.find(record.relative_path);
    if (it != path_to_index_.end()) {
        // 已存在，更新
        records_[it->second] = record;
        LOG_DEBUG("Updated existing record for: {}", record.relative_path);
    } else {
        // 新记录，添加
        path_to_index_[record.relative_path] = records_.size();
        records_.push_back(record);
        LOG_DEBUG("Added new record for: {}", record.relative_path);
    }

    return true;
}

bool UploadProgressManager::updateRecord(const std::string& relative_path,
                                         UploadStatus status,
                                         const std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    auto it = path_to_index_.find(relative_path);
    if (it == path_to_index_.end()) {
        LOG_WARN("Record not found for update: {}", relative_path);
        return false;
    }

    auto& record = records_[it->second];
    record.status = status;
    record.updated_at = std::chrono::system_clock::now();
    if (!error.empty()) {
        record.last_error = error;
    }

    LOG_DEBUG("Updated record status: {} -> {}", relative_path, static_cast<int>(status));

    // 智能保存：仅在超过保存间隔时保存
    if (shouldSave()) {
        cleanRecords();
        bool saved = save();
        return saved;
    }

    return true;
}

UploadRecord* UploadProgressManager::getRecord(const std::string& relative_path) {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    auto it = path_to_index_.find(relative_path);
    if (it == path_to_index_.end()) {
        return nullptr;
    }

    return &records_[it->second];
}

bool UploadProgressManager::isUploaded(const std::string& relative_path) const {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    auto it = path_to_index_.find(relative_path);
    if (it == path_to_index_.end()) {
        return false;
    }

    return records_[it->second].status == UploadStatus::Success;
}

bool UploadProgressManager::isPendingOrUploading(const std::string& relative_path) const {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    auto it = path_to_index_.find(relative_path);
    if (it == path_to_index_.end()) {
        return false;
    }

    const auto& record = records_[it->second];
    return record.status == UploadStatus::Pending ||
           record.status == UploadStatus::Uploading;
}

std::vector<UploadRecord> UploadProgressManager::getPendingRecords() const {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    std::vector<UploadRecord> pending;
    for (const auto& record : records_) {
        if (record.status == UploadStatus::Pending ||
            record.status == UploadStatus::Failed) {
            pending.push_back(record);
        }
    }

    return pending;
}

void UploadProgressManager::cleanRecords(int older_than_hours) {
    std::lock_guard<std::recursive_mutex> lock(records_mutex_);

    size_t removed = 0;
    std::vector<UploadRecord> filtered;
    std::unordered_map<std::string, size_t> new_index;

    for (size_t i = 0; i < records_.size(); ++i) {
        const auto& record = records_[i];

        if (std::filesystem::exists(record.file_path)) {
            new_index[record.relative_path] = filtered.size();
            filtered.push_back(record);
        } else {
            removed++;
        }
    }

    records_ = std::move(filtered);
    path_to_index_ = std::move(new_index);

    LOG_INFO("Cleared {} upload records",
                 removed);
}
