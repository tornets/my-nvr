//
// NPU 推理线程池 - 管理 RKNN 推理 worker，每个绑定一个 NPU 核心
//

#ifndef NVR_DETECTION_POOL_H
#define NVR_DETECTION_POOL_H

#include "detection_types.h"
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <future>

struct AVFrame;

namespace nvr::detection {

class RKNNDetector;

// 池返回的检测结果（含调试图像数据）
struct PoolDetectionResult {
    DetectionResult detection;
    bool success = false;
    bool dropped = false;  // 队列过载时任务被丢弃

#if DUMP_DECTECT_IMAGE
    std::vector<uint8_t> debug_rgb;
    int debug_w = 0;
    int debug_h = 0;
#endif
};

// NPU 推理线程池
class DetectionPool {
public:
    DetectionPool(int num_workers, const DetectionConfig& config, int max_queue_size = 64);
    ~DetectionPool();

    DetectionPool(const DetectionPool&) = delete;
    DetectionPool& operator=(const DetectionPool&) = delete;

    bool initialize();
    void shutdown();

    // 同步检测接口：提交任务到共享队列，阻塞等待结果
    // frame 的所有权不转移，调用者负责释放
    PoolDetectionResult detect(::AVFrame* frame);

    int getNumWorkers() const { return num_workers_; }

private:
    struct Task {
        ::AVFrame* frame;
        std::promise<PoolDetectionResult> promise;
    };

    void workerLoop(int index);

    int num_workers_;
    int max_queue_size_;
    DetectionConfig config_;

    // 共享任务队列
    std::queue<Task> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Worker 线程和对应的检测器
    struct Worker {
        std::unique_ptr<RKNNDetector> detector;
        std::thread thread;
    };
    std::vector<std::unique_ptr<Worker>> workers_;

    std::atomic<bool> running_;
};

} // namespace nvr::detection

#endif // NVR_DETECTION_POOL_H
