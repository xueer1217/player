#include <iostream>
#include "constant.h"
#include <chrono>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include "SDL2/SDL.h"
}


using namespace std;
using namespace chrono;

#define REFRESH_EVENT (SDL_USEREVENT + 1)

#define BREAK_EVENT (SDL_USEREVENT + 2)

bool thread_exit = false;

int event_thread(void *opaque) {

    while (!thread_exit) {
        SDL_Event event;
        event.type = REFRESH_EVENT;
        SDL_PushEvent(&event);
        SDL_Delay(40);
    }

    return 0;
}


int main(int argc, char *argv[]) {

    //ffmpeg
    AVFormatContext *formatContex = nullptr;
    AVCodec *v_codec = nullptr;
    AVCodecContext *v_codec_ctx = nullptr;
    AVFrame *v_fm_raw = nullptr;
    AVFrame *v_fm_yuv = nullptr;
    int buff_size = 0;
    uint8_t *buff = nullptr;
    struct SwsContext *sws_ctx = nullptr;
    int v_idx = -1;
    int a_idx = -1;
    AVPacket *v_pack = nullptr;

    //sdl

    SDL_Window *screen = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_Rect rect;

    //ffmpeg 部分
    if (argc < 2) {
        cout << "please input file " << endl;
        return ERR;
    }

    int ret = avformat_open_input(&formatContex, argv[1], nullptr, nullptr);

    if (ret < 0) {
        cout << "open input error" << endl;
        return ERR;
    }

    ret = avformat_find_stream_info(formatContex, nullptr);
    if (ret < 0) {
        cout << "find stream error" << endl;
        return ERR;
    }

    av_dump_format(formatContex, 0, argv[1], 0);

    for (int i = 0; i < formatContex->nb_streams; i++) {
        if (formatContex->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            v_idx = i;
            cout << "video index is " + to_string(i) << endl;
        }
        if (formatContex->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            a_idx = i;
        }
    }

    if (v_idx == -1) {
        cout << "can not find video stream" << endl;
        return ERR;
    }
    if (a_idx == -1) {
        cout << "can not find audio stream" << endl;
    }

    v_codec = avcodec_find_decoder(formatContex->streams[v_idx]->codecpar->codec_id);

    if (v_codec == nullptr) {
        cout << "can not find codec " << endl;
        return ERR;
    }
    v_codec_ctx = avcodec_alloc_context3(v_codec);
    ret = avcodec_parameters_to_context(v_codec_ctx, formatContex->streams[v_idx]->codecpar);

    if (ret < 0) {
        cout << "avcodec_parameters_to_context err" << endl;
        return ERR;
    }

    ret = avcodec_open2(v_codec_ctx, v_codec, nullptr);

    if (ret < 0) {
        cout << "avcode_open2 err" << endl;
        return ERR;
    }

    int h = v_codec_ctx->height;
    int w = v_codec_ctx->width;

    v_fm_raw = av_frame_alloc();
    v_fm_yuv = av_frame_alloc();

    buff_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, v_codec_ctx->width, v_codec_ctx->height, 1);
    buff = (uint8_t *) av_malloc(buff_size);
    av_image_fill_arrays(v_fm_yuv->data, v_fm_yuv->linesize, buff, AV_PIX_FMT_YUV420P, v_codec_ctx->width,
                         v_codec_ctx->height, 1);

    sws_ctx = sws_getContext(w, h, v_codec_ctx->pix_fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr,
                             nullptr);


    //sdl部分
    //初始化
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {

        cout << "sdl init error" << endl;
        return ERR;
    }

    //创建窗口
    screen = SDL_CreateWindow("player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_SHOWN);

    if (!screen) {
        cout << "create window error" << endl;
        return ERR;
    }


//    SDL_ShowWindow(screen);
//    渲染器
    renderer = SDL_CreateRenderer(screen, -1, 0);
//    创建纹理
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);

    rect.x = 0;
    rect.y = 0;
    rect.w = w;
    rect.h = h;

    v_pack = (AVPacket *) av_malloc(sizeof(AVPacket));

    SDL_Thread *video_tid = SDL_CreateThread(event_thread, NULL, NULL);
    SDL_Event event;
//    //从视频中读取数据 只处理video

    while (true) {
        SDL_WaitEvent(&event);
        if (event.type == REFRESH_EVENT) {
//            auto t1 = system_clock::now();
            while (av_read_frame(formatContex, v_pack)==0) {
                if (v_pack->stream_index == v_idx) {
                    break;
                }

            }
            //video 一个packet中包含一个frame
            //向解码器中喂数据
            ret = avcodec_send_packet(v_codec_ctx, v_pack);
            if (ret < 0) {
                if (ret == AVERROR(EAGAIN)) {
                    cout << "send packet over" << endl;
                    break;
                } else {
                    cout << "send packet error:" << to_string(ret) << endl;
                    break;
                }
            }
            ret = avcodec_receive_frame(v_codec_ctx, v_fm_raw);
            if (ret != 0) {
                cout << "receive frame error:" << to_string(ret) << endl;
                break;
            }
            //图像转换
            sws_scale(sws_ctx,
                      (const uint8_t *const *) v_fm_raw->data,
                      v_fm_raw->linesize,
                      0,
                      v_codec_ctx->height,
                      v_fm_yuv->data,
                      v_fm_yuv->linesize
            );
            SDL_UpdateYUVTexture(
                    texture,
                    &rect,
                    v_fm_yuv->data[0],
                    v_fm_yuv->linesize[0],
                    v_fm_yuv->data[1],
                    v_fm_yuv->linesize[1],
                    v_fm_yuv->data[2],
                    v_fm_yuv->linesize[2]
            );
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, &rect);
            SDL_RenderPresent(renderer);

            av_packet_unref(v_pack);
//            auto t2 = system_clock::now();
//            auto duration = duration_cast<microseconds>(t2-t1);
//            cout <<  "花费了"
//                 << double(duration.count()) * microseconds::period::num / microseconds::period::den
//                 << "秒" << endl;

        } else if (event.type == SDL_QUIT) {
            thread_exit = true;
            break;
        }
//        else if (event.type == BREAK_EVENT) {
//            cout << "break" << endl;
//            break;
//        }

    }

    SDL_Quit();
    sws_freeContext(sws_ctx);
    avcodec_free_context(&v_codec_ctx);
    av_frame_free(&v_fm_raw);
    av_frame_free(&v_fm_yuv);
    av_packet_free(&v_pack);
    av_free(buff);
    avformat_close_input(&formatContex);

    return 0;
}
