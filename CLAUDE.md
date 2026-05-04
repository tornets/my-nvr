# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

MyNVR 是一个用 C++17 编写的跨平台网络视频录制服务，可以从 RTSP 摄像头录制视频流并自动上传到服务器。支持多个同时连接的摄像头。在 RK3588 平台上支持基于 YOLO11 的智能录制——仅录制检测到玩家的视频段。

## 技术栈
- 视频: ffmpeg（RK3588 上使用 ffmpeg-rockchip 硬件加速）
- AI 推理: RKNN (rknpu2) + YOLO11
- 硬件预处理: RGA (Raster Graphics Accelerator)
- 零拷贝: DRM DMA-BUF
- 依赖管理: conan

## 常用命令

```
# 构建
conan build . -of=build -s build_type=Debug -s compiler.cppstd=17 --build=missing

# 运行
build/src/nvr --config config.yaml
```

## 开发工作流

**Windows 服务模式：**
```cmd
nvr.exe service install -- --config config.yaml
nvr.exe service start
nvr.exe service stop
nvr.exe service restart
nvr.exe service uninstall
```

**日志：** 写入可执行文件目录下的 `nvr.log`（控制台模式下为当前工作目录）。

## 架构概览

应用程序采用分层架构：

```
main.cpp (入口点)
    ├── CLI 参数解析 (argparse)
    └── 服务管理命令

Application (application.cpp)
    ├── 日志初始化 (spdlog)
    ├── 信号处理 (SIGINT/SIGTERM)
    └── 配置上传到服务器

NVRManager (nvr_manager.cpp) - 核心协调器
    ├── 管理 IPCRecorder 实例（每个摄像头一个）
    ├── 清理线程（自动删除旧文件）
    └── 上传扫描线程（每 10 秒）

IPCRecorder (ipc_recorder.cpp) - RTSP 录制引擎
    ├── FFmpeg 集成（RTSP/解码/编码）
    ├── 自动重连与超时检测
    ├── 分段分割（默认：10 分钟）
    ├── 两阶段录制（临时目录 → 最终目录）
    └── [RK3588] SmartRecordingManager 集成

SmartRecordingManager (smart_recording_manager.cpp) - 智能录制决策引擎
    ├── 三态状态机 (IDLE → RECORDING → POST_RECORDING)
    ├── 帧预缓存 (FrameBuffer 环形缓冲区)
    ├── 独立检测线程 (生产者-消费者模式)
    └── 延迟停止机制

RKNNDetector (rknn_detector.cpp) - YOLO11 目标检测
    ├── RKNN 模型加载与推理
    ├── 零拷贝路径 (DMA-BUF → RGA → RKNN)
    └── CPU 路径 (AVFrame → RKNN)

DMABufferExtractor (dma_buffer_extractor.cpp) - DMA 缓冲区提取
RgaPreprocessor (rga_preprocessor.cpp) - RGA 硬件预处理
FrameBuffer (frame_buffer.cpp) - 帧环形缓冲区
DetectionResultCache (detection_result_cache.cpp) - 检测结果滑动窗口

VideoUploader (video_uploader.cpp)
    └── 基于队列的 HTTP 多部分上传，支持重试
```

## 主要依赖

### 跨平台依赖（conan 管理）
- **FFmpeg** - 视频/音频编码和 RTSP 流（Windows: 4.4.6，Linux: 系统/ffmpeg-rockchip）
- **spdlog 1.15.1** - 日志记录
- **yaml-cpp 0.8.0** - 配置文件解析
- **cpp-httplib 0.30.1** - HTTP 上传（含 OpenSSL）
- **nlohmann_json 3.11.3** - JSON 序列化

### RK3588 平台依赖（系统库）
- **librknnrt** - RKNN 运行时，模型推理
- **rockchip_mpp** - Media Process Platform，硬件编解码
- **rga** - Raster Graphics Accelerator，硬件图像缩放/格式转换
- **drm** - Direct Rendering Manager，GPU/DMA-BUF 管理

## 条件编译

智能录制通过 `ENABLE_RKNN_SMART_RECORDING` 宏控制，仅在 aarch64（RK3588）平台自动启用。所有智能录制相关代码使用 `#ifdef ENABLE_RKNN_SMART_RECORDING` 包裹，确保在其他平台编译不受影响。

## 线程模型

- **主线程：** 应用程序生命周期循环（100ms 休眠间隔）
- **每个 RTSP 流一个线程** (IPCRecorder::run())
- **[RK3588] 每个流的检测线程** (SmartRecordingManager::detectionWorkerThread())
- **清理线程**（如果启用自动清理）
- **上传扫描线程**（如果启用上传，每 10 秒扫描一次）
- **上传工作线程**（如果启用上传）

## 智能录制详解

### 状态机
```
IDLE (预缓存) ──检测到玩家──> RECORDING (录制中)
  ↑                                │
  │                          玩家消失 + 延迟到期
  │                                ↓
  └──────────────────── POST_RECORDING (延迟停止)
                              │
                        玩家重新出现 → 回到 RECORDING
```

### 预缓存机制
IDLE 状态时，FrameBuffer 维护一个环形缓冲区（默认 5 秒），缓存所有视频帧。当检测到玩家开始录制时，从最近关键帧开始提取预缓存帧写入文件，确保视频从玩家出现前就开始。

### 延迟停止
玩家消失后进入 POST_RECORDING 状态，等待配置的延迟时间（默认 5 秒）。如果延迟期间玩家重新出现，回到 RECORDING 状态；延迟到期则停止录制。

### 检测流程
1. IPCRecorder 对每个关键帧解码得到 AVFrame
2. 调用 DMABufferExtractor 从 AVFrame 提取 DMA-BUF 信息（零拷贝路径）
3. RgaPreprocessor 使用 RGA 硬件将 NV12 缩放/转换为模型输入尺寸
4. RKNNDetector 执行 YOLO11 推理，NMS 后处理
5. 区分玩家（class_id=0）和 NPC（class_id=1）

### 分段决策
DetectionResultCache 维护滑动窗口记录检测结果。分段结束时根据窗口内玩家出现比例决定是否保存该段视频。

## 关键实现细节

### 文件管理
- **两阶段录制：** 文件首先写入临时目录，然后移动到最终目录。这防止上传不完整的文件。
- **启动清理：** 启动时清理临时目录，删除之前运行的残留文件。
- **多级目录支持：** 文件跟踪使用相对路径以支持嵌套目录结构。

### 重连逻辑
- 可配置的重连间隔（默认：5 秒）
- 最大重连次数（-1 表示无限制）
- 使用 FFmpeg 中断回调进行超时检测 (`ipc_recorder.cpp:interrupt_callback`)
- 每个流的重连计数器在 `IPCRecorder` 中跟踪

### 文件名模板
录制文件名使用模板变量：
- `{stream_id}` - 摄像头/流标识符
- `{stream_name}` - 流名称
- `{shop_id}` - 店铺 ID
- `{start_datetime}` - 录制开始时间
- `{segment_index}` - 该流的段编号
- `{duration}` - 录制时长
- `{end_time}` - 录制结束时间

## 配置结构

主配置文件是 `config.yaml`（模板见 `config.yaml.example`）：

- **streams[]** - RTSP 流配置（url、timeout、reconnect 设置）
  - **streams[].smart_recording** - 智能录制配置（仅 RK3588）
    - enabled, prebuffer_duration_seconds, segment_duration_seconds
    - min_recording_duration, post_recording_delay_seconds
    - **rknn** - RKNN 推理配置（model_path, confidence_threshold, detection_interval_keyframes, zero_copy_enabled）
- **record** - 输出目录、分段时长、文件名模板
- **autoclean** - 文件时长限制、磁盘使用限制
- **upload** - HTTP URL、超时、重试设置
- **shop** - 用于服务器注册的商店 ID
- **log_level** - trace/debug/info/warn/error/critical

## 重要文件位置

- `src/main.cpp` - 入口点、服务命令
- `src/application.cpp` - 应用初始化和主循环
- `src/nvr_manager.cpp` - 核心管理器
- `src/ipc_recorder.cpp` - RTSP 录制引擎
- `src/video_uploader.cpp` - 上传功能
- `src/config_loader.cpp` - YAML 配置解析
- `src/cmdline_parser.cpp` - CLI 参数解析
- `src/win32_service.cpp` - Windows 服务包装器
- `src/smart_recording_manager.cpp` - 智能录制决策引擎（RK3588）
- `src/rknn_detector.cpp` - YOLO11 RKNN 检测器（RK3588）
- `src/dma_buffer_extractor.cpp` - DMA 缓冲区提取（RK3588）
- `src/rga_preprocessor.cpp` - RGA 硬件预处理（RK3588）
- `src/frame_buffer.cpp` - 帧环形缓冲区（RK3588）
- `src/detection_result_cache.cpp` - 检测结果缓存（RK3588）
- `src/detection_types.h` - 检测相关类型定义
- `models/yolo11.rknn` - YOLO11 RKNN 模型文件

## Windows 服务集成

- 服务名称：`NVRService`
- 显示名称：`NVR Video Recorder Service`
- 通过 `win32_service.cpp` 使用 Win32 Service API
- 默认服务账户：LocalSystem
- 服务回调：on_start、on_stop、on_pause、on_continue

## 代码规范

- C++17 标准
- 此仓库中常见中文提交信息
- 基于异常的错误处理
- 需要的地方使用互斥锁进行线程安全操作
- 日志使用 spdlog，格式：`[YYYY-MM-DD HH:MM::SS.mmm] [logger] [LEVEL] message`
- RK3588 相关代码使用 `#ifdef ENABLE_RKNN_SMART_RECORDING` 条件编译
