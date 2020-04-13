//
// Created by 郝雪 on 2020/4/12.
//


extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
#include "SDL2/SDL.h"

}

#include <iostream>
#include <constant.h>

#define REFRESH_EVENT (SDL_USEREVENT + 1)
using namespace std;

struct packet_queue {
    AVPacketList *first_pakcet;
    AVPacketList *last_packet;
    int nb_pakcet;
    int size; //queue中packet总的大小
    SDL_mutex *mutex;
    SDL_cond *cond;
};

packet_queue a_pkt_queue, v_pkt_queue;
bool vdecode_finish = false;
bool adecode_finish = false;
SDL_Window *screen;

void packet_queue_init(packet_queue *queue) {
    memset(queue, 0, sizeof(packet_queue));
    queue->mutex = SDL_CreateMutex(); //创建一个互斥对象并初始化成解锁状态
    queue->cond = SDL_CreateCond();
}

//写到队列尾部
int packet_queue_push(packet_queue *queue, AVPacket *packet) {

    AVPacketList *packet_list = (AVPacketList *) av_malloc(sizeof(AVPacketList));

    if (packet_list == nullptr) {
        cout << "av_malloc error" << endl;
        return ERR;
    }
    if (av_packet_make_refcounted(packet) < 0) {
        cout << "packet is not reference-counted" << endl;
        return ERR;
    }

    packet_list->pkt = *packet;
    packet_list->next = nullptr;
    SDL_LockMutex(queue->mutex);

    if (queue->last_packet == nullptr) {
        queue->first_pakcet = packet_list;
    } else {
        queue->last_packet->next = packet_list;
    }
    queue->last_packet = packet_list;
    queue->nb_pakcet++;
    queue->size += packet->size;
    SDL_CondSignal(queue->cond);
    SDL_UnlockMutex(queue->mutex);

    return 0;
}

//取队列头部
int packet_queue_pop(packet_queue *queue, AVPacket *packet, int block) {
    AVPacketList *queue_pop = nullptr;
    int ret = -1;
    SDL_LockMutex(queue->mutex);
    while (true) {
        queue_pop = queue->first_pakcet;

        if (queue_pop != nullptr) { // 队列非空

            queue->first_pakcet = queue_pop->next;
            if (queue->first_pakcet == nullptr) {
                queue->last_packet = nullptr;
            }
            queue->nb_pakcet--;
            queue->size -= queue_pop->pkt.size;
            *packet = queue_pop->pkt;
            av_free(queue_pop);
            ret = 1;
            break;
        } else if (!block) { //队列空并且阻塞标志无效
            ret = 0;
            break;
        } else { //队列空，输入没结束，阻塞标志有效则等待
            SDL_CondWait(queue->cond, queue->mutex);
        }
    }

    SDL_UnlockMutex(queue->mutex);
    return ret;

}


int init_codec_ctx(AVFormatContext *f_ctx, AVCodecContext **c_ctx, int idx) {

    AVCodecParameters *codec_para = f_ctx->streams[idx]->codecpar;
    int codec_type = codec_para->codec_type;
    AVCodec *codec = avcodec_find_decoder(codec_para->codec_id);
    if (codec == nullptr) {
        cout << "av_codec_decoder error,idx" << idx << endl;
        return ERR;
    }
    *c_ctx = avcodec_alloc_context3(codec);
    if (c_ctx == nullptr) {
        cout << "avcodec_alloc_context3 error,idx" << idx << endl;
        return ERR;
    }
    if (avcodec_parameters_to_context(*c_ctx, codec_para) < 0) {
        cout << "avcodec_parameters_to_context error,idx" << idx << endl;
        return ERR;
    }
    if (avcodec_open2(*c_ctx, codec, nullptr) < 0) {
        cout << "avcodec_open2 error,idx" << idx << endl;
        return ERR;
    }

    return 0;

}

uint32_t video_thread_timer(uint32_t interval, void *param) {

    SDL_Event event;
    event.type = REFRESH_EVENT;
    SDL_PushEvent(&event);
    return interval;
}

int video_thread(void *data) {

    AVCodecContext *codec_ctx = (AVCodecContext *) data;
    //frame_yuv,frame_src 用来存储解码后图像转换前后的frame ,buffer 是frame_yuv的缓冲区
    int h = codec_ctx->height;
    int w = codec_ctx->width;

    AVFrame *fm_raw = av_frame_alloc();
    AVFrame *fm_yuv = av_frame_alloc();

    if (fm_raw == nullptr || fm_yuv == nullptr) {
        cout << "av_frame_alloc error" << endl;
        return -1;
    }
    int buff_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
    uint8_t *buff = (uint8_t *) av_malloc(buff_size);
    if (buff == nullptr) {
        cout << "av_malloc error" << endl;
        av_frame_free(&fm_yuv);
        av_frame_free(&fm_raw);
        return ERR;
    }
    if (av_image_fill_arrays(fm_yuv->data, fm_yuv->linesize, buff, AV_PIX_FMT_YUV420P, w, h, 1) < 0) {
        cout << "av_image_fill_arrays error" << endl;
        av_frame_free(&fm_yuv);
        av_frame_free(&fm_raw);
        av_free(buff);
        return ERR;
    }

    //初始化 sws_ctx
    SwsContext *sws_ctx = sws_getContext(w, h, codec_ctx->pix_fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr,
                                         nullptr,
                                         nullptr);

    if (sws_ctx == nullptr) {
        cout << "sws_getContext error" << endl;
        av_frame_free(&fm_yuv);
        av_frame_free(&fm_raw);
        av_free(buff);
        return ERR;
    }

    //创建sdl window render texture rect
    //todo 目前creat window 放在子线程中
//
//    SDL_Window *screen = SDL_CreateWindow("player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, w, h,
//                                          SDL_WINDOW_SHOWN);
//
//    if (!screen) {
//        cout << "create window error" <<SDL_GetError()<< endl;
//        return ERR;
//    }

    SDL_Renderer *renderer = SDL_CreateRenderer(screen, -1, 0);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);

    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = w;
    rect.h = h;

    AVPacket *packet = (AVPacket *) av_malloc(sizeof(AVPacket));

    int ret = 0;
    SDL_Event event;
    //读取packet 解码
    while (true) {
        if (vdecode_finish) {
            break;
        }
        if (packet_queue_pop(&v_pkt_queue, packet, 1) <= 0) {
            cout << "video packet queue is empty.." << endl;
            SDL_Delay(1000);
            continue;
        }
        ret = avcodec_send_packet(codec_ctx, packet);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                cout << "input is not accepted in the current state" << endl;
            } else if (ret == AVERROR_EOF) {
                cout << "the decoder has been flushed" << endl;
            } else {
                cout << "avcodec_send_packet error" << to_string(ret) << endl;
                av_packet_unref(packet);
                return ERR;
            }
        }
        av_packet_unref(packet);

        ret = avcodec_receive_frame(codec_ctx, fm_raw);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN)) {
                cout << "output is not available in this state" << endl;
            } else if (ret == AVERROR_EOF) {
                cout << "the decoder has been fully flushed" << endl;
                vdecode_finish = true;
            } else {
                cout << "avcodec_receive_packet error" << endl;
                return ERR;
            }
        }
        //图像转换
        sws_scale(sws_ctx,
                  (const uint8_t *const *) fm_raw->data,
                  fm_raw->linesize,
                  0,
                  h,
                  fm_yuv->data,
                  fm_yuv->linesize
        );
        SDL_UpdateYUVTexture(
                texture,
                &rect,
                fm_yuv->data[0],
                fm_yuv->linesize[0],
                fm_yuv->data[1],
                fm_yuv->linesize[1],
                fm_yuv->data[2],
                fm_yuv->linesize[2]
        );
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, &rect);
        SDL_RenderPresent(renderer);

        SDL_WaitEvent(&event);

    }


}

int packet_thread(void *data) {

    AVFormatContext* format_ctx= (AVFormatContext*) data;



}

int open_video_stream(AVFormatContext *f_ctx, AVCodecContext **c_ctx, int idx) {


    //初始化codec_ctx
    init_codec_ctx(f_ctx, c_ctx, idx);
    //创建定时器和解码线程
    // 帧率为：avg_frame_rate.num / avg_frame_rate.den

    int num = f_ctx->streams[idx]->avg_frame_rate.num;
    int den = f_ctx->streams[idx]->avg_frame_rate.den;
    int frame_rate = (den > 0) ? num / den : 25;
    int interval = (num > 0) ? (den * 1000) / num : 40;

    cout << "frame rate is" + to_string(frame_rate) + " fps and interval is " + to_string(interval) + " ms" << endl;
    //为解码线程的定时器
    SDL_AddTimer(interval, video_thread_timer, nullptr);
    SDL_CreateThread(video_thread, "video_thread", *c_ctx);

    return 0;

}

int open_audio_stream(AVFormatContext *f_ctx, AVCodecContext *c_ctx, int idx) {

}


//packet
//主线程处理video packet
//packet线程 获取packet
//audio thream 处理audio packet
int main(int argc, char *argv[]) {

    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *v_codec_ctx = nullptr;
    AVCodecContext *a_codec_ctx = nullptr;
    AVPacket *pakcet = nullptr;
    int v_idx = -1;
    int a_idx = -1;

    int ret = 0;
    if (argc < 2) {
        cout << "you should input file name" << endl;
        return ERR;
    }

    if (avformat_open_input(&format_ctx, argv[1], nullptr, nullptr) < 0) {
        cout << "open input error" << endl;
        return ERR;
    }
    if (avformat_find_stream_info(format_ctx, nullptr)< 0) {
        cout << "find stream info error "<< endl;
        return ERR;
    }
    av_dump_format(format_ctx, 0, argv[1], 0);

    for (int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            a_idx = i;
        }
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            v_idx = i;
        }
    }
    if (a_idx == -1) {
        cout << "find audio stream error" << endl;
        return ERR;
    }
    if (v_idx == -1) {
        cout << "find video stream error" << endl;
        return ERR;
    }

    SDL_CreateThread(packet_thread,"packet_thread", format_ctx);



    //init sdl
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        cout << "sdl init error" << endl;
        return ERR;
    }


    screen = SDL_CreateWindow("player", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, format_ctx->streams[v_idx]->codecpar->width, format_ctx->streams[v_idx]->codecpar->height,
            SDL_WINDOW_SHOWN);


    if (!screen) {
        cout << "create window error" <<SDL_GetError()<< endl;
        return ERR;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(screen, -1, 0);
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h);

    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = w;
    rect.h = h;


    open_video_stream(format_ctx, &v_codec_ctx, v_idx);

    AVPacket* packet = (AVPacket*)av_malloc(sizeof(AVPacket));

    long long idx = 0;
    //读取数据
    while (true) {
        ret = av_read_frame(format_ctx, packet);
//        cout<<idx++<<endl;
        if(ret<0){
            if(ret ==AVERROR_EOF){
                cout<<"read end od file"<<endl;
                av_packet_unref(packet);
                packet = nullptr;

                packet_queue_push(&v_pkt_queue,packet);
                packet_queue_push(&a_pkt_queue,packet);
                break;
            }else{
                cout<<"av_read_frame error"<<endl;
                return ERR;
            }
        }else{

            if(packet->stream_index == v_idx){
                packet_queue_push(&v_pkt_queue,packet);
            }else if(packet->stream_index == a_idx){
                packet_queue_push(&a_pkt_queue,packet);
            }else{
                av_packet_unref(packet);
            }

        }

    }

    while(!vdecode_finish){
        SDL_Delay(1000);
    }


}