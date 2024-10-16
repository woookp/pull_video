#include <iostream>
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include <SDL2/SDL.h>

std::mutex frameMutex;
std::condition_variable frameCondVar;
std::deque<AVFrame*> frameQueue;
bool frameReady = false;
bool quit = false;
const size_t MAX_FRAMES = 1;

void cleanup_frame(AVFrame* frame) {
    if (frame) {
        av_frame_free(&frame);
    }
}

// 清空帧队列的函数
void clear_frame_queue() {
    std::lock_guard<std::mutex> lock(frameMutex);
    while (!frameQueue.empty()) {
        cleanup_frame(frameQueue.front());
        frameQueue.pop_front();
    }
}

// 打开RTSP流的封装函数
int open_rtsp_stream(AVFormatContext** formatContext, const char* rtspurl) {
    AVDictionary* options = NULL;
    av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP传输

    if (avformat_open_input(formatContext, rtspurl, NULL, &options) != 0) {
        std::cerr << "Error: Couldn't open the RTSP stream." << std::endl;
        av_dict_free(&options);
        return -1;
    }
    av_dict_free(&options);

    if (avformat_find_stream_info(*formatContext, NULL) < 0) {
        std::cerr << "Error: Couldn't find stream information." << std::endl;
        return -1;
    }
    return 0;
}

int main() {
    // 初始化 FFmpeg
    av_register_all();
    avformat_network_init();

    const char* rtspurl = "rtsp://192.168.107.200:8554/stream";

    AVFormatContext* formatContext = avformat_alloc_context();
    if (open_rtsp_stream(&formatContext, rtspurl) != 0) {
        return -1;
    }

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

    AVCodecParameters* codecParameters = formatContext->streams[videoStream]->codecpar;
    AVCodec* codec = avcodec_find_decoder(codecParameters->codec_id);
    AVCodecContext* codecContext = avcodec_alloc_context3(codec);
    if (!codecContext || avcodec_parameters_to_context(codecContext, codecParameters) < 0 || avcodec_open2(codecContext, codec, NULL) < 0) {
        std::cerr << "Error: Couldn't initialize codec." << std::endl;
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

    SwsContext* swsContext = sws_getContext(codecContext->width, codecContext->height, codecContext->pix_fmt,
        codecContext->width, codecContext->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, codecContext->width, codecContext->height, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
    av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, buffer, AV_PIX_FMT_RGB24, codecContext->width, codecContext->height, 1);

    AVPacket packet;

    // 创建解码线程
    std::thread decodeThread([&]() {
        while (!quit) {
            if (av_read_frame(formatContext, &packet) >= 0) {
                if (packet.stream_index == videoStream) {
                    if (avcodec_send_packet(codecContext, &packet) == 0) {
                        while (avcodec_receive_frame(codecContext, frame) == 0) {
                            std::unique_lock<std::mutex> lock(frameMutex);
                            if (!frameQueue.empty()) {
                                cleanup_frame(frameQueue.front());
                                frameQueue.pop_front();
                            }
                            frameQueue.push_back(av_frame_clone(frame));  // 存储成功解码的帧
                            frameReady = true;
                            frameCondVar.notify_one();  // 通知渲染线程
                        }
                    }
                }
                av_packet_unref(&packet);
            } else {
                SDL_Delay(10);
                std::cerr << "Warning: Error reading frame, attempting reconnection..." << std::endl;

                // 重连逻辑
                avformat_close_input(&formatContext);
                formatContext = avformat_alloc_context();
                clear_frame_queue();  // 清空帧队列
                while (open_rtsp_stream(&formatContext, rtspurl) != 0) {
                    std::cerr << "Reconnection failed, retrying in 5 seconds..." << std::endl;
                    SDL_Delay(5000);  // 等待5秒再重试
                }
                std::cerr << "Reconnected successfully." << std::endl;
            }
        }
    });

    SDL_Event event;
    while (!quit) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            }
        }

        // 等待解码线程解码出一帧
        std::unique_lock<std::mutex> lock(frameMutex);
        frameCondVar.wait(lock, [] { return frameReady || quit; });

        if (!quit && !frameQueue.empty()) {
            AVFrame* currentFrame = frameQueue.front();
            frameQueue.pop_front();

            // 转换为 RGB 格式
            sws_scale(swsContext, currentFrame->data, currentFrame->linesize, 0, codecContext->height, rgbFrame->data, rgbFrame->linesize);

            // 更新纹理并显示
            SDL_UpdateTexture(texture, NULL, rgbFrame->data[0], rgbFrame->linesize[0]);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);

            cleanup_frame(currentFrame);
            frameReady = false;  // 重置状态
        }
    }

    // 释放资源
    av_free(buffer);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    avcodec_free_context(&codecContext);
    avformat_close_input(&formatContext);
    sws_freeContext(swsContext);
    decodeThread.join();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
