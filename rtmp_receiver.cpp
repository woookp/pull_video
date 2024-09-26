#include <iostream>
#include <string>
#include <deque>
#include <mutex>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <SDL2/SDL.h>

std::mutex frameMutex;
std::deque<AVFrame*> frameQueue;  // 存储解码成功的帧
const size_t MAX_FRAMES = 10;  // 设置最大帧数

void cleanup_frame(AVFrame* frame) {
    if (frame) {
        av_frame_free(&frame);
    }
}

int main() {
    // 初始化 FFmpeg
    av_register_all();
    avformat_network_init();

    const char* rtspurl = "rtsp://192.168.78.200:8554/stream"; // 不加 ?tcp

    // 设置RTSP选项，使用TCP传输
    AVDictionary* options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);

    // 打开 RTSP 流
    AVFormatContext* formatContext = avformat_alloc_context();
    if (avformat_open_input(&formatContext, rtspurl, NULL, &options) != 0) {
        std::cerr << "Error: Couldn't open the RTSP stream." << std::endl;
        return -1;
    }
    av_dict_free(&options);  // 释放选项

    if (avformat_find_stream_info(formatContext, NULL) < 0) {
        std::cerr << "Error: Couldn't find stream information." << std::endl;
        return -1;
    }

    // 找到视频流
    int videoStream = -1;
    for (int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }

    if (videoStream == -1) {
        std::cerr << "Error: Couldn't find a video stream." << std::endl;
        return -1;
    }

    // 初始化解码器
    AVCodecParameters* codecParameters = formatContext->streams[videoStream]->codecpar;
    AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
    AVCodecContext* codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        std::cerr << "Error: Couldn't allocate codec context." << std::endl;
        return -1;
    }

    if (avcodec_parameters_to_context(codecContext, codecParameters) < 0) {
        std::cerr << "Error: Couldn't copy codec parameters to context." << std::endl;
        return -1;
    }

    if (avcodec_open2(codecContext, codec, NULL) < 0) {
        std::cerr << "Error: Couldn't open codec." << std::endl;
        return -1;
    }

    // 初始化 SDL2
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "Error: Couldn't initialize SDL - " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("RTSP Stream", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        codecContext->width, codecContext->height, SDL_WINDOW_OPENGL);
    if (!window) {
        std::cerr << "Error: Couldn't create SDL window - " << SDL_GetError() << std::endl;
        return -1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        codecContext->width, codecContext->height);

    // 创建 SwsContext 用于格式转换
    SwsContext* swsContext = sws_getContext(codecContext->width, codecContext->height, codecContext->pix_fmt,
        codecContext->width, codecContext->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codecContext->width, codecContext->height, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_RGB24, codecContext->width, codecContext->height, 1);

    AVPacket packet;
    bool quit = false;
    SDL_Event event;

    // 创建一个线程用于解码
    std::thread decodeThread([&]() {
        while (!quit) {
            if (av_read_frame(formatContext, &packet) >= 0) {
                if (packet.stream_index == videoStream) {
                    // 发送数据包到解码器
                    if (avcodec_send_packet(codecContext, &packet) == 0) {
                        while (avcodec_receive_frame(codecContext, frame) == 0) {
                            std::lock_guard<std::mutex> lock(frameMutex);
                            // 限制帧队列的大小
                            if (frameQueue.size() >= MAX_FRAMES) {
                                cleanup_frame(frameQueue.front());
                                frameQueue.pop_front();
                            }
                            frameQueue.push_back(av_frame_clone(frame));  // 存储成功解码的帧
                        }
                    }
                }
                av_packet_unref(&packet);
            } else {
                SDL_Delay(10);  // 暂停一小段时间
            }
        }
    });

    while (!quit) {
        // 处理 SDL 事件
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            }
        }

        std::lock_guard<std::mutex> lock(frameMutex);
        if (!frameQueue.empty()) {
            AVFrame* currentFrame = frameQueue.front();
            frameQueue.pop_front();

            // 将解码后的帧转换为 RGB 格式
            sws_scale(swsContext, currentFrame->data, currentFrame->linesize, 0, codecContext->height, rgbFrame->data, rgbFrame->linesize);

            // 更新 SDL 纹理
            SDL_UpdateTexture(texture, NULL, rgbFrame->data[0], rgbFrame->linesize[0]);

            // 清除渲染器并绘制纹理
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);

            cleanup_frame(currentFrame);  // 清理当前帧
        } else {
            SDL_Delay(10);  // 暂停一小段时间
        }
    }

    // 释放资源
    av_free(buffer);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);
    sws_freeContext(swsContext);
    decodeThread.join();  // 等待解码线程结束

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
