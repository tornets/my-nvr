# NVR Service 安装指南

## 系统要求

- Windows 10/11 (64位)
- 管理员权限
- 至少 1GB 可用磁盘空间（根据录制需求增加）

## 安装步骤

### 方法 1：使用安装程序（推荐）

1. 运行 `nvr-service-setup-1.0.0.exe`
2. 选择安装路径（默认：`C:\Program Files\NVR Service`）
3. 安装程序会自动：
   - 复制程序文件
   - 创建必要的目录
   - 安装 Windows 服务
   - 创建开始菜单快捷方式

4. 安装完成后：
   - 编辑配置文件 `config.yaml`
   - 重启服务使配置生效

### 方法 2：手动安装

```cmd
# 1. 创建安装目录
mkdir C:\NVR
cd C:\NVR

# 2. 复制文件
# 将以下文件复制到 C:\NVR
# - nvr.exe
# - config.yaml
# - 所有依赖的 DLL 文件

# 3. 安装服务
nvr.exe service install -- --config C:\NVR\config.yaml

# 4. 启动服务
nvr.exe service start

# 5. 检查服务状态
sc query NVRService
```

## 配置

编辑 `config.yaml` 文件配置 NVR 服务：

```yaml
# 流配置
streams:
  - id: camera1
    url: rtsp://username:password@192.168.1.100/stream
    params:
      timeout: 30
      reconnect: auto

# 录制配置
record:
  output_dir: "C:/NVR/recordings"
  segment_duration_seconds: 600  # 10分钟
  filename_template: "{stream_id}_{start_datetime}_{duration}_{end_time}.mp4"

# 日志配置
log_level: "debug"  # trace, debug, info, warn, error, critical

# 上传配置（可选）
upload:
  enabled: true
  url: "https://your-server.com/api/upload"
  timeout_seconds: 300
  max_retries: 3

# 自动清理配置
autoclean:
  enabled: true
  max_age_hours: 168  # 7天
  max_disk_usage_gb: 100
```

## 服务管理命令

### 命令行工具

```cmd
# 查看服务状态
nvr service status

# 启动服务
nvr service start

# 停止服务
nvr service stop

# 重启服务
nvr service restart

# 卸载服务
nvr service uninstall
```

### Windows 服务管理器

```cmd
# 使用 sc 命令
sc query NVRService          # 查询状态
sc start NVRService          # 启动
sc stop NVRService           # 停止
sc config NVRService         # 查看配置
```

或使用 Windows 服务管理器 (`services.msc`)：

1. 按 `Win + R`，输入 `services.msc`
2. 找到 "NVR Video Recorder Service"
3. 右键可以启动/停止/重启服务

## 日志

日志文件位置：
- 服务模式：`安装目录\nvr.log`
- 控制台模式：当前工作目录 `nvr.log`

查看日志：
```cmd
# Windows 命令行
type "C:\Program Files\NVR Service\nvr.log"

# PowerShell
Get-Content "C:\Program Files\NVR Service\nvr.log" -Tail 50 -Wait
```

## 卸载

### 方法 1：使用控制面板

1. 打开 "控制面板" → "程序和功能"
2. 找到 "NVR Service"
3. 点击 "卸载"

### 方法 2：使用安装程序

运行安装程序 `nvr-service-setup-1.0.0.exe`，选择 "删除/卸载"

### 方法 3：手动卸载

```cmd
# 1. 停止服务
nvr service stop

# 2. 卸载服务
nvr service uninstall

# 3. 删除文件
rd /s /q "C:\Program Files\NVR Service"
```

## 故障排除

### 服务无法启动

1. 检查配置文件语法是否正确
2. 查看日志文件获取详细错误信息
3. 确认 RTSP 流地址可访问
4. 确保有足够的磁盘空间

### 服务停止超时

如果服务停止时报错 1053：

```cmd
# 强制结束进程
taskkill /F /IM nvr.exe

# 重启服务
nvr service start
```

### 录制文件未生成

1. 检查录制目录权限
2. 检查磁盘空间
3. 查看 RTSP 流是否正常连接
4. 检查日志中的错误信息

## 高级配置

### 更改服务运行账户

默认以 LocalSystem 运行，如需更改：

```cmd
sc config NVRService obj=.\username password=password
```

### 配置服务恢复选项

```cmd
# 失败后自动重启
sc failure NVRService reset= 86400 actions= restart/60000
```

## 命令行参数

```cmd
# 控制台模式（用于调试）
nvr.exe --config config.yaml

# 指定日志级别
nvr.exe --config config.yaml --log-level trace

# 覆盖配置参数
nvr.exe --config config.yaml --record-output-dir "D:\Recordings"
```

## 技术支持

- GitHub: https://github.com/yourusername/nvr
- Issues: https://github.com/yourusername/nvr/issues
- 文档: https://github.com/yourusername/nvr/wiki

## 许可证

[您的许可证信息]
