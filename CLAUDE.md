# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

MyNVR 是一个用 C++17 编写的 Windows 网络视频录制服务，可以从 RTSP 摄像头录制视频流并自动上传到服务器。它作为 Windows 后台服务运行，支持多个同时连接的摄像头。

## 构建命令

```cmd
# Debug 构建
build.bat debug

# Release 构建
build.bat release

# 构建安装程序（支持 WiX 4.x 和 Inno Setup 6.x）
build_installer.bat          # 默认：WiX
build_installer.bat --wix
build_installer.bat --innosetup
build_installer.bat --skip-build    # 跳过构建，仅创建安装程序

# 输出位置：
# - 可执行文件：build\src\Debug\nvr.exe 或 build\src\Release\nvr.exe
# - 安装程序（WiX）：build\*.msi
# - 安装程序（Inno）：installer_output\nvr-service-setup-1.0.0.exe
```

**前置要求：**
- Visual Studio 2022 (17.x)
- CMake 3.27+
- Conan 包管理器
- WiX Toolset 4.x 或 Inno Setup 6.x（用于安装程序）

## 开发工作流

**控制台模式（用于调试）：**
```cmd
build\src\Debug\nvr.exe --config config.yaml
```

**服务模式：**
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
    └── 两阶段录制（临时目录 → 最终目录）

VideoUploader (video_uploader.cpp)
    └── 基于队列的 HTTP 多部分上传，支持重试
```

## 主要依赖

- **FFmpeg 8.0.1** - 视频/音频编码和 RTSP 流
- **spdlog 1.15.1** - 日志记录
- **yaml-cpp 0.8.0** - 配置文件解析
- **cpp-httplib 0.30.1** - HTTP 上传（含 OpenSSL）
- **nlohmann_json 3.11.3** - JSON 序列化

## 线程模型

- **主线程：** 应用程序生命周期循环（100ms 休眠间隔）
- **每个 RTSP 流一个线程** (IPCRecorder::run())
- **清理线程**（如果启用自动清理）
- **上传扫描线程**（如果启用上传，每 10 秒扫描一次）
- **上传工作线程**（如果启用上传）

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
- `{start_datetime}` - 录制开始时间
- `{segment_index}` - 该流的段编号
- `{duration}` - 录制时长
- `{end_time}` - 录制结束时间

## 配置结构

主配置文件是 `config.yaml`（模板见 `config.yaml.example`）：

- **streams[]** - RTSP 流配置（url、timeout、reconnect 设置）
- **record** - 输出目录、分段时长、文件名模板
- **autoclean** - 文件时长限制、磁盘使用限制
- **upload** - HTTP URL、超时、重试设置
- **shop** - 用于服务器注册的商店 ID
- **log_level** - trace/debug/info/warn/error/critical

## 重要文件位置

- `src/main.cpp` - 入口点、服务命令
- `src/application.cpp` - 应用初始化和主循环
- `src/nvr_manager.cpp` - 核心管理器（446 行）
- `src/ipc_recorder.cpp` - RTSP 录制引擎（900+ 行）
- `src/video_uploader.cpp` - 上传功能
- `src/win32_service.cpp` - Windows 服务包装器
- `src/config_loader.cpp` - YAML 配置解析
- `src/cmdline_parser.cpp` - CLI 参数解析

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
