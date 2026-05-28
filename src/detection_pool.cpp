//
// NPU 推理线程池实现
//

#include "detection_pool.h"
#include "rknn_detector.h"
#include "dma_buffer_extractor.h"
#include "log.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/hwcontext.h>
}

namespace nvr::detection {

DetectionPool::DetectionPool(int num_workers, const DetectionConfig& config, int max_queue_size)
    : num_workers_(num_workers)
    , max_queue_size_(max_queue_size)
    , config_(config)
    , running_(false)
{
    if (num_workers_ <= 0) num_workers_ = 3;
    if (max_queue_size_ <= 0) max_queue_size_ = 64;
}

DetectionPool::~DetectionPool() {
    shutdown();
}

bool DetectionPool::initialize() {
    LOG_INFO("DetectionPool: initializing {} workers", num_workers_);

    // NPU 核心掩码：RKNN_NPU_CORE_0=1, CORE_1=2, CORE_2=4
    static const uint32_t core_masks[] = {1, 2, 4};

    for (int i = 0; i < num_workers_; i++) {
        auto w = std::make_unique<Worker>();

        // 创建 detector
        w->detector = std::make_unique<RKNNDetector>(config_);
        if (!w->detector->initialize()) {
            LOG_ERROR("DetectionPool: worker {} detector init failed", i);
            return false;
        }

        // 绑定 NPU 核心
        uint32_t mask = (i < 3) ? core_masks[i] : 1;
        if (!w->detector->setCoreMask(mask)) {
            LOG_ERROR("DetectionPool: worker {} set core mask failed", i);
            return false;
        }

        // 预热
        if (!w->detector->warmup()) {
            LOG_WARN("DetectionPool: worker {} warmup failed", i);
        }

        LOG_INFO("DetectionPool: worker {} initialized, NPU core mask={}", i, mask);

        workers_.push_back(std::move(w));
    }

    running_ = true;

    // 启动 worker 线程
    for (int i = 0; i < num_workers_; i++) {
        workers_[i]->thread = std::thread(&DetectionPool::workerLoop, this, i);
    }

    LOG_INFO("DetectionPool: started with {} workers", num_workers_);
    return true;
}

void DetectionPool::shutdown() {
    if (!running_) return;

    running_ = false;
    queue_cv_.notify_all();

    // 等待线程结束
    for (auto& w : workers_) {
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }

    // 兑现残留任务的 promise（避免调用者 future.get() 抛异常）
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!task_queue_.empty()) {
            try {
                task_queue_.front().promise.set_value({});
            } catch (...) {}
            task_queue_.pop();
        }
    }

    for (auto& w : workers_) {
        w->detector.reset();
    }
    workers_.clear();
    LOG_INFO("DetectionPool: shutdown complete");
}

PoolDetectionResult DetectionPool::detect(AVFrame* frame, int64_t frame_pts) {
    if (!running_ || !frame) {
        return {};
    }

    Task task;
    task.frame = frame;
    task.frame_pts = frame_pts;
    auto future = task.promise.get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        // 队列积压超过最大容量时丢弃最旧任务
        size_t max_queue = static_cast<size_t>(max_queue_size_);
        while (task_queue_.size() >= max_queue) {
            try {
                PoolDetectionResult dropped_result;
                dropped_result.dropped = true;
                task_queue_.front().promise.set_value(std::move(dropped_result));
            } catch (...) {}
            task_queue_.pop();
        }
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();

    return future.get();
}

void DetectionPool::workerLoop(int index) {
    auto& detector = workers_[index]->detector;
    LOG_DEBUG("DetectionPool worker {} started", index);

    while (running_) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !running_ || !task_queue_.empty();
            });

            if (!running_ && task_queue_.empty()) break;
            if (task_queue_.empty()) continue;

            task = std::move(task_queue_.front());
            task_queue_.pop();
        }

        PoolDetectionResult result;

        // 根据配置选择 CPU 或零拷贝模式
        if (config_.zero_copy_enabled && task.frame->format == AV_PIX_FMT_DRM_PRIME) {
            // 零拷贝模式：直接使用 DMA fd
            auto wrapper = DMABufferExtractor::extractFromAVFrame(task.frame);
            if (wrapper && wrapper->isValid()) {
                result.success = detector->detectFrameZeroCopy(
                    wrapper->getInfo(), result.detection, task.frame_pts);
            } else {
                // 零拷贝失败，回退到 CPU 模式
                result.success = detector->detectFrame(task.frame, result.detection, task.frame_pts);
            }
        } else if (task.frame->format == AV_PIX_FMT_DRM_PRIME) {
            // CPU 模式但收到 DRM_PRIME 帧：先下载到 CPU 内存
            AVFrame* cpu_frame = av_frame_alloc();
            if (cpu_frame) {
                // 从硬件帧下载到 CPU 内存
                int ret = av_hwframe_transfer_data(cpu_frame, task.frame, 0);
                if (ret == 0) {
                    cpu_frame->width = task.frame->width;
                    cpu_frame->height = task.frame->height;
                    result.success = detector->detectFrame(cpu_frame, result.detection, task.frame_pts);
                } else {
                    LOG_ERROR("av_hwframe_transfer_data failed: {}", ret);
                }
                av_frame_free(&cpu_frame);
            }
        } else {
            // 普通 CPU 帧（非 DRM_PRIME）
            result.success = detector->detectFrame(task.frame, result.detection, task.frame_pts);
        }

        if (config_.dump_detect.enable && result.success) {
            result.debug_rgb = detector->getLastInputRGB();
            result.debug_w = detector->getLastInputWidth();
            result.debug_h = detector->getLastInputHeight();
            LOG_DEBUG("[POOL] Worker {} captured debug_rgb: {} bytes, first bytes: [{},{},{},{},{},{}]",
                      index, result.debug_rgb.size(),
                      result.debug_rgb.empty() ? 0 : result.debug_rgb[0],
                      result.debug_rgb.empty() ? 0 : result.debug_rgb[1],
                      result.debug_rgb.empty() ? 0 : result.debug_rgb[2],
                      result.debug_rgb.empty() ? 0 : result.debug_rgb[3],
                      result.debug_rgb.empty() ? 0 : result.debug_rgb[4],
                      result.debug_rgb.empty() ? 0 : result.debug_rgb[5]);
        }

        task.promise.set_value(std::move(result));
    }

    LOG_DEBUG("DetectionPool worker {} stopped", index);
}

} // namespace nvr::detection
