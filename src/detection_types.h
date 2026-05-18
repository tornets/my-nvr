//
// Created by Claude on 2026/4/19.
// 检测相关类型定义
//

#ifndef NVR_DETECTION_TYPES_H
#define NVR_DETECTION_TYPES_H

#include <vector>
#include <chrono>
#include <cstdint>
#include <string>

namespace nvr::detection {

// DMA Buffer 信息
struct DMABufferInfo {
    int fd;                // DMA-BUF 文件描述符
    size_t size;          // 缓冲区大小
    int width;            // 宽度
    int height;           // 高度
    int format;           // 格式
    int stride;           // 行字节数（stride/pitch），0 表示等于 width
    int height_stride;    // 高度方向 stride（含对齐填充），0 表示等于 height

    DMABufferInfo() : fd(-1), size(0), width(0), height(0), format(0), stride(0), height_stride(0) {}
};

// Letterbox 预处理参数（完全符合 Rockchip 官方标准）
struct LetterboxParams {
    float scale;      // 缩放比例 = min(dst_width/src_width, dst_height/src_height)
    int pad_x;        // X 方向填充偏移（左边距）
    int pad_y;        // Y 方向填充偏移（上边距）

    LetterboxParams() : scale(1.0f), pad_x(0), pad_y(0) {}
};

// 边界框
struct BoundingBox {
    float x;          // 左上角 x 坐标
    float y;          // 左上角 y 坐标
    float width;      // 宽度
    float height;     // 高度
    float confidence; // 置信度
    int class_id;     // 类别 ID

    BoundingBox(float x_ = 0, float y_ = 0, float w = 0, float h = 0,
               float conf = 0, int id = -1)
        : x(x_), y(y_), width(w), height(h), confidence(conf), class_id(id) {}

    // 检查点是否在边界框内
    bool contains(float px, float py) const {
        return px >= x && px <= (x + width) && py >= y && py <= (y + height);
    }

    // 获取中心点
    std::pair<float, float> center() const {
        return std::make_pair(x + width / 2.0f, y + height / 2.0f);
    }

    // 计算与另一个边界框的 IoU (Intersection over Union)
    float iou(const BoundingBox& other) const {
        float x1 = std::max(x, other.x);
        float y1 = std::max(y, other.y);
        float x2 = std::min(x + width, other.x + other.width);
        float y2 = std::min(y + height, other.y + other.height);

        if (x2 <= x1 || y2 <= y1) {
            return 0.0f;
        }

        float intersection = (x2 - x1) * (y2 - y1);
        float area1 = width * height;
        float area2 = other.width * other.height;
        float union_area = area1 + area2 - intersection;

        return intersection / union_area;
    }
};

// 检测结果
struct DetectionResult {
    bool has_player;                    // 是否检测到玩家
    bool has_npc;                       // 是否检测到 NPC
    float player_confidence;            // 玩家最大置信度
    float npc_confidence;               // NPC 最大置信度
    std::vector<BoundingBox> boxes;     // 所有检测到的边界框
    int player_class_id = 1;            // 玩家类别 ID
    int npc_class_id = 0;               // NPC 类别 ID
    int64_t frame_pts;                  // 帧 PTS 时间戳
    double processing_time_ms;          // 处理耗时（毫秒）
    std::chrono::system_clock::time_point timestamp; // 检测时间戳

    DetectionResult()
        : has_player(false)
        , has_npc(false)
        , player_confidence(0.0f)
        , npc_confidence(0.0f)
        , frame_pts(0)
        , processing_time_ms(0.0)
        , timestamp(std::chrono::system_clock::now()) {}

    void clear() {
        has_player = false;
        has_npc = false;
        player_confidence = 0.0f;
        npc_confidence = 0.0f;
        boxes.clear();
        frame_pts = 0;
        processing_time_ms = 0.0;
        timestamp = std::chrono::system_clock::now();
    }

    // 获取指定类别的所有边界框
    std::vector<BoundingBox> getBoxesByClass(int class_id) const {
        std::vector<BoundingBox> result;
        for (const auto& box : boxes) {
            if (box.class_id == class_id) {
                result.push_back(box);
            }
        }
        return result;
    }

    // 获取玩家边界框
    std::vector<BoundingBox> getPlayerBoxes() const {
        return getBoxesByClass(player_class_id);
    }

    // 获取 NPC 边界框
    std::vector<BoundingBox> getNPCBoxes() const {
        return getBoxesByClass(npc_class_id);
    }
};

// 检测统计信息
struct DetectionStats {
    int total_frames_processed;        // 总处理帧数
    int player_detected_frames;        // 检测到玩家的帧数
    int npc_detected_frames;           // 检测到 NPC 的帧数
    double total_processing_time_ms;   // 总处理时间（毫秒）
    double avg_processing_time_ms;     // 平均处理时间（毫秒）
    double max_processing_time_ms;     // 最大处理时间（毫秒）
    double min_processing_time_ms;     // 最小处理时间（毫秒）
    std::chrono::system_clock::time_point last_update; // 最后更新时间

    DetectionStats()
        : total_frames_processed(0)
        , player_detected_frames(0)
        , npc_detected_frames(0)
        , total_processing_time_ms(0.0)
        , avg_processing_time_ms(0.0)
        , max_processing_time_ms(0.0)
        , min_processing_time_ms(999999.0)
        , last_update(std::chrono::system_clock::now()) {}

    void update(const DetectionResult& result) {
        total_frames_processed++;
        total_processing_time_ms += result.processing_time_ms;

        if (result.has_player) {
            player_detected_frames++;
        }
        if (result.has_npc) {
            npc_detected_frames++;
        }

        // 更新最小/最大处理时间
        if (result.processing_time_ms > max_processing_time_ms) {
            max_processing_time_ms = result.processing_time_ms;
        }
        if (result.processing_time_ms < min_processing_time_ms) {
            min_processing_time_ms = result.processing_time_ms;
        }

        // 计算平均处理时间
        if (total_frames_processed > 0) {
            avg_processing_time_ms = total_processing_time_ms / total_frames_processed;
        }

        last_update = std::chrono::system_clock::now();
    }

    void reset() {
        total_frames_processed = 0;
        player_detected_frames = 0;
        npc_detected_frames = 0;
        total_processing_time_ms = 0.0;
        avg_processing_time_ms = 0.0;
        max_processing_time_ms = 0.0;
        min_processing_time_ms = 999999.0;
        last_update = std::chrono::system_clock::now();
    }

    // 获取玩家检测率
    float getPlayerDetectionRate() const {
        if (total_frames_processed == 0) {
            return 0.0f;
        }
        return static_cast<float>(player_detected_frames) / total_frames_processed;
    }

    // 获取 NPC 检测率
    float getNPCDetectionRate() const {
        if (total_frames_processed == 0) {
            return 0.0f;
        }
        return static_cast<float>(npc_detected_frames) / total_frames_processed;
    }
};

// 调试图像导出配置
enum class DumpDetectFilter { All, HasDetection, NoDetection };

struct DumpDetectConfig {
    bool enable = false;
    DumpDetectFilter filter = DumpDetectFilter::All;
};

// 检测配置
struct DetectionConfig {
    std::string model_path;             // RKNN 模型路径
    int player_class_id;                // 玩家类别 ID
    int npc_class_id;                   // NPC 类别 ID
    float confidence_threshold;         // 置信度阈值
    int detection_interval_keyframes;   // 检测间隔（关键帧数）
    bool zero_copy_enabled;             // 零拷贝开关
    DumpDetectConfig dump_detect;       // 调试图像导出配置

    DetectionConfig()
        : player_class_id(1)
        , npc_class_id(0)
        , confidence_threshold(0.5f)
        , detection_interval_keyframes(30)
        , zero_copy_enabled(true) {}
};

} // namespace nvr::detection

#endif // NVR_DETECTION_TYPES_H
