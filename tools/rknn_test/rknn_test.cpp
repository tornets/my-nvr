//
// RKNN 推理测试工具
// 用于测试 YOLO11 目标检测模型
//

#include "rknn_detector.h"
#include "detection_types.h"
#include "log.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <cstring>
#include <fstream>

using namespace nvr::detection;

// ============================================================================
// 字体和绘制函数（复用 smart_recording_manager.cpp）
// ============================================================================

// 5×7 位图字体（每个字符 7 字节，每字节低 5 位为一行）
struct Glyph5x7 { char ch; uint8_t rows[7]; };

static const Glyph5x7 FONT[] = {
    {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    {'2',{0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}},
    {'3',{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}},
    {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
    {'.',{0x00,0x00,0x00,0x00,0x00,0x06,0x06}},
    {':',{0x00,0x00,0x06,0x00,0x06,0x00,0x00}},
    {'-',{0x00,0x00,0x00,0x1F,0x1F,0x00,0x00}},
    {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
    {'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11}},
    {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    {'a',{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}},
    {'e',{0x00,0x00,0x0E,0x11,0x1E,0x10,0x0E}},
    {'l',{0x08,0x08,0x08,0x08,0x08,0x08,0x08}},
    {'r',{0x00,0x00,0x0E,0x11,0x1E,0x10,0x10}},
    {'y',{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}},
    {' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
    {'(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02}},
    {')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08}},
    {'[',{0x06,0x04,0x04,0x04,0x04,0x04,0x06}},
    {']',{0x0C,0x04,0x04,0x04,0x04,0x04,0x0C}},
    {'x',{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}},
    {'/',{0x01,0x02,0x04,0x08,0x10,0x20,0x00}},
    {'%',{0x11,0x0A,0x04,0x0A,0x11,0x00,0x00}},
    {'+',{0x00,0x00,0x04,0x1F,0x04,0x00,0x00}},
    {'=',{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}},
    {'<',{0x02,0x04,0x08,0x10,0x08,0x04,0x02}},
    {'>',{0x08,0x04,0x02,0x01,0x02,0x04,0x08}},
};

const Glyph5x7* findGlyph(char c) {
    for (const auto& g : FONT) {
        if (g.ch == c) return &g;
    }
    return nullptr;
}

void drawRect(std::vector<uint8_t>& rgb, int img_w, int img_h,
              int rx, int ry, int rw, int rh, int thickness,
              uint8_t r, uint8_t g, uint8_t b) {
    for (int t = 0; t < thickness; t++) {
        // 上边
        for (int x = rx; x < rx + rw && x < img_w; x++) {
            int py = ry + t;
            if (py >= 0 && py < img_h && x >= 0) {
                int idx = (py * img_w + x) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
        // 下边
        for (int x = rx; x < rx + rw && x < img_w; x++) {
            int py = ry + rh - 1 - t;
            if (py >= 0 && py < img_h && x >= 0) {
                int idx = (py * img_w + x) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
        // 左边
        for (int y = ry; y < ry + rh && y < img_h; y++) {
            int px = rx + t;
            if (px >= 0 && px < img_w && y >= 0) {
                int idx = (y * img_w + px) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
        // 右边
        for (int y = ry; y < ry + rh && y < img_h; y++) {
            int px = rx + rw - 1 - t;
            if (px >= 0 && px < img_w && y >= 0) {
                int idx = (y * img_w + px) * 3;
                rgb[idx] = r; rgb[idx+1] = g; rgb[idx+2] = b;
            }
        }
    }
}

void drawText(std::vector<uint8_t>& rgb, int img_w, int img_h,
              const std::string& text, int x, int y, int scale,
              uint8_t r, uint8_t g, uint8_t b) {
    int cx = x;
    for (char c : text) {
        const Glyph5x7* gl = findGlyph(c);
        if (!gl) { cx += (6 * scale); continue; }
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (gl->rows[row] & (0x10 >> col)) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = cx + col * scale + sx;
                            int py = y + row * scale + sy;
                            if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                                int idx = (py * img_w + px) * 3;
                                rgb[idx] = r;
                                rgb[idx+1] = g;
                                rgb[idx+2] = b;
                            }
                        }
                    }
                }
            }
        }
        cx += 6 * scale;
    }
}

int measureText(const std::string& text, int scale) {
    return static_cast<int>(text.size()) * 6 * scale;
}

// ============================================================================
// 图像加载函数
// ============================================================================

bool loadImageFromFile(const std::string& filename,
                       std::vector<uint8_t>& rgb_data,
                       int& width, int& height) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVCodec* codec = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frame_rgb = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws_ctx = nullptr;
    bool success = false;
    int video_stream_index = -1;
    AVStream* video_stream = nullptr;
    int ret;

    // 打开输入文件
    if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "Error: Cannot open image file: " << filename << std::endl;
        return false;
    }

    // 获取流信息
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "Error: Cannot find stream info" << std::endl;
        goto cleanup;
    }

    // 查找视频流
    video_stream_index = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO,
                                              -1, -1, &codec, 0);
    if (video_stream_index < 0) {
        std::cerr << "Error: No video stream found" << std::endl;
        goto cleanup;
    }

    // 创建解码器上下文
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Error: Cannot allocate codec context" << std::endl;
        goto cleanup;
    }

    video_stream = format_ctx->streams[video_stream_index];
    if (avcodec_parameters_to_context(codec_ctx, video_stream->codecpar) < 0) {
        std::cerr << "Error: Cannot copy codec parameters" << std::endl;
        goto cleanup;
    }

    // 打开解码器
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Error: Cannot open codec" << std::endl;
        goto cleanup;
    }

    // 分配帧和包
    frame = av_frame_alloc();
    frame_rgb = av_frame_alloc();
    packet = av_packet_alloc();

    if (!frame || !frame_rgb || !packet) {
        std::cerr << "Error: Cannot allocate frame or packet" << std::endl;
        goto cleanup;
    }

    // 读取第一帧
    ret = av_read_frame(format_ctx, packet);
    if (ret < 0) {
        std::cerr << "Error: Cannot read frame" << std::endl;
        goto cleanup;
    }

    // 发送包到解码器
    if (avcodec_send_packet(codec_ctx, packet) < 0) {
        std::cerr << "Error: Cannot send packet to decoder" << std::endl;
        goto cleanup;
    }

    // 接收解码后的帧
    ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret < 0) {
        std::cerr << "Error: Cannot receive frame from decoder" << std::endl;
        goto cleanup;
    }

    width = frame->width;
    height = frame->height;

    // 分配 RGB 缓冲区
    frame_rgb->format = AV_PIX_FMT_RGB24;
    frame_rgb->width = width;
    frame_rgb->height = height;
    if (av_frame_get_buffer(frame_rgb, 0) < 0) {
        std::cerr << "Error: Cannot allocate RGB frame buffer" << std::endl;
        goto cleanup;
    }

    // 转换像素格式
    sws_ctx = sws_getContext(
        width, height, static_cast<AVPixelFormat>(frame->format),
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx) {
        std::cerr << "Error: Cannot create SWS context" << std::endl;
        goto cleanup;
    }

    sws_scale(sws_ctx, frame->data, frame->linesize, 0, height,
              frame_rgb->data, frame_rgb->linesize);

    // 复制 RGB 数据
    rgb_data.resize(width * height * 3);
    for (int y = 0; y < height; y++) {
        std::memcpy(rgb_data.data() + y * width * 3,
                    frame_rgb->data[0] + y * frame_rgb->linesize[0],
                    width * 3);
    }

    success = true;

cleanup:
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (packet) av_packet_free(&packet);
    if (frame_rgb) av_frame_free(&frame_rgb);
    if (frame) av_frame_free(&frame);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (format_ctx) avformat_close_input(&format_ctx);

    return success;
}

// ============================================================================
// JPG 保存函数
// ============================================================================

bool saveJPG(const std::string& filename,
             const std::vector<uint8_t>& rgb, int width, int height, int quality) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    const AVCodec* codec = nullptr;
    AVStream* stream = nullptr;
    SwsContext* sws_ctx = nullptr;
    const uint8_t* src_data[1];
    int src_linesize[1];
    bool success = false;

    // 查找 MJPEG 编码器
    codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        std::cerr << "Error: Cannot find MJPEG encoder" << std::endl;
        return false;
    }

    // 分配编码器上下文
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Error: Cannot allocate codec context" << std::endl;
        return false;
    }

    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = {1, 25};
    codec_ctx->framerate = {25, 1};
    codec_ctx->pix_fmt = AV_PIX_FMT_YUVJ422P;
    // quality 参数在新版 FFmpeg 中已移除，使用全局质量标志
    codec_ctx->flags |= AV_CODEC_FLAG_QSCALE;
    codec_ctx->global_quality = FF_QP2LAMBDA * quality / 100;

    // 打开编码器
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        std::cerr << "Error: Cannot open codec" << std::endl;
        goto cleanup;
    }

    // 创建输出格式
    if (avformat_alloc_output_context2(&format_ctx, nullptr, nullptr,
                                        filename.c_str()) < 0) {
        std::cerr << "Error: Cannot create output format context" << std::endl;
        goto cleanup;
    }

    // 添加视频流
    stream = avformat_new_stream(format_ctx, nullptr);
    if (!stream) {
        std::cerr << "Error: Cannot create stream" << std::endl;
        goto cleanup;
    }

    // 复制编码器参数到流
    if (avcodec_parameters_from_context(stream->codecpar, codec_ctx) < 0) {
        std::cerr << "Error: Cannot copy codec parameters" << std::endl;
        goto cleanup;
    }

    stream->time_base = codec_ctx->time_base;

    // 打开输出文件
    if (!(format_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&format_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Error: Cannot open output file: " << filename << std::endl;
            goto cleanup;
        }
    }

    // 写入文件头
    if (avformat_write_header(format_ctx, nullptr) < 0) {
        std::cerr << "Error: Cannot write header" << std::endl;
        goto cleanup;
    }

    // 分配帧
    frame = av_frame_alloc();
    frame->format = codec_ctx->pix_fmt;
    frame->width = width;
    frame->height = height;

    if (av_frame_get_buffer(frame, 0) < 0) {
        std::cerr << "Error: Cannot allocate frame buffer" << std::endl;
        goto cleanup;
    }

    // 将 RGB 转换为 YUVJ422P
    sws_ctx = sws_getContext(
        width, height, AV_PIX_FMT_RGB24,
        width, height, AV_PIX_FMT_YUVJ422P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx) {
        std::cerr << "Error: Cannot create SWS context" << std::endl;
        goto cleanup;
    }

    src_data[0] = rgb.data();
    src_linesize[0] = width * 3;

    sws_scale(sws_ctx, src_data, src_linesize, 0, height,
              frame->data, frame->linesize);

    sws_freeContext(sws_ctx);
    sws_ctx = nullptr;

    frame->pts = 0;

    // 编码帧
    if (avcodec_send_frame(codec_ctx, frame) < 0) {
        std::cerr << "Error: Cannot send frame to encoder" << std::endl;
        goto cleanup;
    }

    // 接收编码后的包
    packet = av_packet_alloc();
    if (avcodec_receive_packet(codec_ctx, packet) < 0) {
        std::cerr << "Error: Cannot receive packet from encoder" << std::endl;
        goto cleanup;
    }

    packet->stream_index = stream->index;
    av_packet_rescale_ts(packet, codec_ctx->time_base, stream->time_base);

    // 写入包
    if (av_interleaved_write_frame(format_ctx, packet) < 0) {
        std::cerr << "Error: Cannot write frame" << std::endl;
        goto cleanup;
    }

    // 写入文件尾
    av_write_trailer(format_ctx);
    success = true;

cleanup:
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (format_ctx) {
        if (!(format_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&format_ctx->pb);
        }
        avformat_free_context(format_ctx);
    }

    return success;
}

// ============================================================================
// 打印帮助信息
// ============================================================================

void printUsage(const char* program_name) {
    std::cout << "RKNN Inference Test Tool\n"
              << "Usage: " << program_name << " --model <model_path> --image <image_path> [options]\n\n"
              << "Required:\n"
              << "  --model <path>       RKNN model file path\n"
              << "  --image <path>       Test image file path\n\n"
              << "Options:\n"
              << "  --output <path>      Output image file path, JPG format (default: result.jpg)\n"
              << "  --confidence <float> Confidence threshold (default: 0.5)\n"
              << "  --player-id <int>    Player class ID (default: 1)\n"
              << "  --npc-id <int>       NPC class ID (default: 0)\n"
              << "  --warmup <int>       Warmup iterations (default: 3)\n"
              << "  --help               Show this help message\n";
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char** argv) {
    std::string model_path;
    std::string image_path;
    std::string output_path = "result.jpg";
    float confidence_threshold = 0.5f;
    int player_class_id = 1;
    int npc_class_id = 0;
    int warmup_iterations = 3;
    bool use_raw_rgb = false;
    int raw_width = 640;
    int raw_height = 640;

    // 解析命令行参数
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--model" && i + 1 < argc) {
            model_path = argv[++i];
        } else if (arg == "--image" && i + 1 < argc) {
            image_path = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--confidence" && i + 1 < argc) {
            confidence_threshold = std::stof(argv[++i]);
        } else if (arg == "--player-id" && i + 1 < argc) {
            player_class_id = std::stoi(argv[++i]);
        } else if (arg == "--npc-id" && i + 1 < argc) {
            npc_class_id = std::stoi(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup_iterations = std::stoi(argv[++i]);
        } else if (arg == "--rgb") {
            use_raw_rgb = true;
        } else if (arg == "--rgb-size" && i + 2 < argc) {
            raw_width = std::stoi(argv[++i]);
            raw_height = std::stoi(argv[++i]);
            warmup_iterations = std::stoi(argv[++i]);
        } else {
            std::cerr << "Error: Unknown or incomplete argument: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
    }

    // 验证必需参数
    if (model_path.empty() || image_path.empty()) {
        std::cerr << "Error: --model and --image are required" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    // 检查文件是否存在
    if (!std::filesystem::exists(model_path)) {
        std::cerr << "Error: Model file not found: " << model_path << std::endl;
        return 1;
    }

    if (!std::filesystem::exists(image_path)) {
        std::cerr << "Error: Image file not found: " << image_path << std::endl;
        return 1;
    }

    // 打印配置信息
    std::cout << "\nRKNN Inference Test Tool\n"
              << "=========================\n"
              << "Model: " << model_path << "\n"
              << "Image: " << image_path << "\n"
              << "Output: " << output_path << "\n"
              << "Confidence Threshold: " << confidence_threshold << "\n"
              << "Player Class ID: " << player_class_id << "\n"
              << "NPC Class ID: " << npc_class_id << "\n"
              << "Warmup Iterations: " << warmup_iterations << "\n" << std::endl;

    // 加载图像
    std::cout << "Loading image..." << std::endl;
    std::vector<uint8_t> rgb_data;
    int img_width = 0, img_height = 0;

    if (use_raw_rgb) {
        // 直接加载原始 RGB 数据
        std::ifstream f(image_path, std::ios::binary);
        if (!f) {
            std::cerr << "Error: Cannot open RGB file: " << image_path << std::endl;
            return 1;
        }
        img_width = raw_width;
        img_height = raw_height;
        rgb_data.resize(img_width * img_height * 3);
        f.read(reinterpret_cast<char*>(rgb_data.data()), rgb_data.size());
        if (!f) {
            std::cerr << "Error: Failed to read RGB data" << std::endl;
            return 1;
        }
        std::cout << "Raw RGB loaded: " << img_width << "x" << img_height << std::endl;
    } else {
        // 从图像文件加载
        if (!loadImageFromFile(image_path, rgb_data, img_width, img_height)) {
            std::cerr << "Error: Failed to load image" << std::endl;
            return 1;
        }
        std::cout << "Image loaded: " << img_width << "x" << img_height << std::endl;
    }

    // 将 RGB 数据转换为 AVFrame
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = img_width;
    frame->height = img_height;

    if (av_frame_get_buffer(frame, 0) < 0) {
        std::cerr << "Error: Cannot allocate frame buffer" << std::endl;
        av_frame_free(&frame);
        return 1;
    }

    // 填充 AVFrame
    for (int y = 0; y < img_height; y++) {
        std::memcpy(frame->data[0] + y * frame->linesize[0],
                    rgb_data.data() + y * img_width * 3,
                    img_width * 3);
    }

    // 创建检测器配置
    DetectionConfig config;
    config.model_path = model_path;
    config.player_class_id = player_class_id;
    config.npc_class_id = npc_class_id;
    config.confidence_threshold = confidence_threshold;
    config.zero_copy_enabled = false;  // 测试工具使用 CPU 模式
    config.detection_interval_keyframes = 1;

    // 创建检测器
    std::cout << "\nInitializing detector..." << std::endl;
    auto detector = std::make_unique<RKNNDetector>(config);

    if (!detector->initialize()) {
        std::cerr << "Error: Failed to initialize detector" << std::endl;
        av_frame_free(&frame);
        return 1;
    }

    // 打印模型信息
    const auto& model_info = detector->getModelInfo();
    std::cout << "Model loaded successfully\n"
              << "Model Input: " << model_info.input_width << "x"
              << model_info.input_height << "x" << model_info.input_channels << "\n"
              << "Number of outputs: " << model_info.num_outputs << std::endl;

    // 预热
    if (warmup_iterations > 0) {
        std::cout << "\nWarming up (" << warmup_iterations << " iterations)..." << std::endl;
        detector->warmup(warmup_iterations);
    }

    // 执行推理
    std::cout << "\nRunning inference..." << std::endl;
    DetectionResult result;

    auto start_time = std::chrono::high_resolution_clock::now();

    if (!detector->detectFrame(frame, result)) {
        std::cerr << "Error: Detection failed" << std::endl;
        detector->shutdown();
        av_frame_free(&frame);
        return 1;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double processing_time = std::chrono::duration<double, std::milli>(
        end_time - start_time).count();

    // 打印检测结果
    std::cout << "\n=========================\n"
              << "Detection Results:\n"
              << "=========================\n"
              << "- Total boxes: " << result.boxes.size() << "\n"
              << "- Player detected: " << (result.has_player ? "YES" : "NO");
    if (result.has_player) {
        std::cout << " (max confidence: " << result.player_confidence << ")";
    }
    std::cout << "\n- NPC detected: " << (result.has_npc ? "YES" : "NO");
    if (result.has_npc) {
        std::cout << " (max confidence: " << result.npc_confidence << ")";
    }
    std::cout << "\n\nBoxes:" << std::endl;

    for (size_t i = 0; i < result.boxes.size(); i++) {
        const auto& box = result.boxes[i];
        std::string class_name = (box.class_id == player_class_id) ? "Player" :
                                 (box.class_id == npc_class_id) ? "NPC" :
                                 "Class(" + std::to_string(box.class_id) + ")";

        std::cout << "  [" << i << "] Class: " << class_name << " (" << box.class_id << "), "
                  << "Conf: " << std::fixed << std::setprecision(3) << box.confidence << "\n"
                  << "      Box: [x=" << static_cast<int>(box.x)
                  << ", y=" << static_cast<int>(box.y)
                  << ", w=" << static_cast<int>(box.width)
                  << ", h=" << static_cast<int>(box.height) << "]\n";
    }

    std::cout << "\nProcessing time: " << std::fixed << std::setprecision(1)
              << processing_time << " ms" << std::endl;

    // 在图像上绘制检测框
    if (!result.boxes.empty()) {
        std::cout << "\nDrawing detection boxes..." << std::endl;

        const int box_thickness = std::max(2, img_width / 400);
        const int font_scale = std::max(1, img_width / 640);

        for (const auto& box : result.boxes) {
            // 确定颜色
            uint8_t r, g, b;
            std::string label;

            if (box.class_id == player_class_id) {
                r = 0; g = 255; b = 0;  // 绿色
                label = "Player";
            } else if (box.class_id == npc_class_id) {
                r = 255; g = 100; b = 0;  // 橙色
                label = "NPC";
            } else {
                r = 255; g = 0; b = 0;  // 红色
                label = "Cls" + std::to_string(box.class_id);
            }

            // 绘制边界框
            int bx = static_cast<int>(box.x);
            int by = static_cast<int>(box.y);
            int bw = static_cast<int>(box.width);
            int bh = static_cast<int>(box.height);

            drawRect(rgb_data, img_width, img_height, bx, by, bw, bh, box_thickness, r, g, b);

            // 绘制标签背景
            std::string text = label + " " + std::to_string(static_cast<int>(box.confidence * 100)) + "%";
            int text_width = measureText(text, font_scale);
            int label_height = 8 * font_scale;

            int label_x = bx;
            int label_y = by - label_height - 2;
            if (label_y < 0) label_y = by + 2;

            // 绘制标签背景
            for (int py = label_y; py < label_y + label_height && py < img_height; py++) {
                for (int px = label_x; px < label_x + text_width && px < img_width; px++) {
                    int idx = (py * img_width + px) * 3;
                    rgb_data[idx] = r;
                    rgb_data[idx + 1] = g;
                    rgb_data[idx + 2] = b;
                }
            }

            // 绘制标签文字
            drawText(rgb_data, img_width, img_height, text,
                    label_x + 1, label_y + 1, font_scale, 255, 255, 255);
        }
    }

    // 保存图像
    std::cout << "Saving output to: " << output_path << std::endl;
    if (!saveJPG(output_path, rgb_data, img_width, img_height, 85)) {
        std::cerr << "Error: Failed to save output image" << std::endl;
    } else {
        std::cout << "Output saved successfully!" << std::endl;
    }

    // 清理
    detector->shutdown();
    av_frame_free(&frame);

    return 0;
}
