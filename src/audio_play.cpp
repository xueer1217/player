//
// Created by 郝雪 on 2020/4/8.
//
#include <iostream>
#include "constant.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include"libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "SDL2/SDL.h"
}
using namespace std;
//这两个大小只是粗略估计
#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000


struct packet_queue {
    AVPacketList *first_pakcet;
    AVPacketList *last_packet;
    int nb_pakcet;
    int size; //queue中packet总的大小
    SDL_mutex *mutex;
    SDL_cond *cond;
};

//为什么要设置这些参数
struct ff_audio_para {
    int freq;
    int channels;
    __int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;

};


packet_queue packet_queue_a;
ff_audio_para audio_para_src;
ff_audio_para audio_para_tgt;
struct SwrContext *swr_ctx;
uint8_t *resample_buff; //重采样输出缓冲区
unsigned int resample_buff_len = 0;//重采样输出缓冲区长度
bool input_finish;
bool decode_finish;


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
        } else if (input_finish) { //队列空了并且输入结束
            ret = 0;
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


/*
 *
 * FFmpeg音频解码后的数据是存放在AVFrame结构中的。
 * Packed格式，frame.data[0]或frame.extended_data[0]包含所有的音频数据中。
 * Planar格式，frame.data[i]或者frame.extended_data[i]表示第i个声道的数据（假设声道0是第一个
 * AVFrame.data数组大小固定为8，如果声道数超过8，需要从frame.extended_data获取声道数据。
*/
int audio_decode_frame(AVCodecContext *codec_ctx, AVPacket *packet, uint8_t *buff, int buff_size) {

    AVFrame *frame = av_frame_alloc();
    bool needNewPakcet = false;
    int ret = -1;
    int nb_resample; //每次重采样后的单声道样本数
    uint8_t *c_buff;
    int c_buff_size;
    while (true) {
        needNewPakcet = false;
        //判断是否需要packet
        ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret != 0) {

            if (ret == AVERROR(EAGAIN)) {
                needNewPakcet = true;
            } else if (ret == AVERROR_EOF) {
                cout << "decoder has been flushed" << endl;
                av_frame_free(&frame);
                return ret;
            } else {
                cout << "avcodec_receive_frame error" << endl;
                av_frame_free(&frame);
                return ret;
            }
        } else {
            //解码之前先判断是否需要重采样
            if (frame->channel_layout != audio_para_src.channel_layout ||
                frame->format != audio_para_src.fmt ||
                frame->sample_rate != audio_para_src.freq) {
                swr_free(&swr_ctx);
                swr_ctx = swr_alloc_set_opts(nullptr,
                                             audio_para_tgt.channel_layout,
                                             audio_para_tgt.fmt,
                                             audio_para_tgt.freq,
                                             frame->channel_layout,
                                             static_cast<AVSampleFormat>(frame->format),
                                             frame->sample_rate,
                                             0,
                                             nullptr
                );

                if (swr_ctx == nullptr || swr_init(swr_ctx) < 0) {
                    cout << "swr_alloc_set_opts error or swr init error" << endl;
                    swr_free(&swr_ctx);
                    return ERR;
                }

                //一个音频流中各参数都一致 因此只用修改一次就可以
                audio_para_src.channel_layout = frame->channel_layout;
                audio_para_src.fmt = static_cast<AVSampleFormat>(frame->format);
                audio_para_src.freq = frame->sample_rate;
            }

            if (swr_ctx != nullptr) { //重采样
                //重采样输入参数1:输入data buffer
                const uint8_t **in = (const uint8_t **) frame->extended_buf;
                //重采样输出参数2：单声道的available样本数
                int out_count = (int64_t) frame->nb_samples * audio_para_tgt.freq / frame->sample_rate + 256;
                //重采样输出参数1：buffer
                uint8_t **out = &resample_buff;
                int out_size = av_samples_get_buffer_size(nullptr, audio_para_tgt.channels, out_count,
                                                          audio_para_tgt.fmt, 0);
                if (out_size < 0) {
                    cout << "av_samples_get_buffer_size error" << endl;
                    av_frame_free(&frame);
                    return out_size;
                }
                //第一次的时候分配给resample_buff一块地址
                if (resample_buff == nullptr) {
                    av_fast_malloc(resample_buff, &resample_buff_len, out_size);
                }
                if (resample_buff == nullptr) {
                    cout << "av_fast_malloc error" << endl;
                    av_frame_free(&frame);
                    return ERR;
                };
                nb_resample = swr_convert(swr_ctx,
                                          out,
                                          out_count,
                                          in,
                                          frame->nb_samples
                );
                if (nb_resample < 0) {
                    cout << "swr_convert error" << endl;
                    av_frame_free(&frame);
                    return ERR;
                }
                if (nb_resample == out_count) {
                    cout << "audio buffer is too small" << endl;
                }

                c_buff = resample_buff;
                c_buff_size = audio_para_tgt.channels * nb_resample * av_get_bytes_per_sample(audio_para_tgt.fmt);
            } else {
                c_buff = frame->data[0];
                //todo 验证这个是否可以
                c_buff_size = av_samples_get_buffer_size(nullptr, frame->channels, frame->nb_samples,
                                                         static_cast<AVSampleFormat>(frame->format), 1);
            }
            memcpy(buff, c_buff, c_buff_size);
            av_frame_free(&frame);
            return c_buff_size;
        }
        if (needNewPakcet) {
            ret = avcodec_send_packet(codec_ctx, packet);
            if (ret != 0) {
                cout << "avcodec_send_packet error" << endl;
                av_frame_free(&frame);
                av_packet_unref(packet);
                return ret;
            }
        }

    }
}

//sdl回调函数：格式固定
//读队列、解码、播放
//stream音频缓冲区地址；将解码之后的音频数据填入此缓冲区；len缓冲区大小，单位字节；
void sdl_audio_callback(void *userdata, uint8_t *stream, int len) {
    AVCodecContext *c_ctx = (AVCodecContext *) userdata;

    uint8_t audio_buff[(MAX_AUDIO_FRAME_SIZE * 3) / 2];//保存每次解码之后的数据
    int copy_len = 0;//每次发送给缓冲区的大小
    static uint32_t audio_len = 0; //新取到的已解码数据大小
    static uint32_t send_len = 0; // 已发送数据大小

    AVPacket *packet;
    int get_size = 0; //packet解码之后的音频数据大小
    while (len > 0) { //直到缓冲区填满之后该函数返回
        if (decode_finish) {
            return;
        }
        if (send_len >= audio_len) {
            packet = (AVPacket *) av_malloc(sizeof(AVPacket));
            //从队列中取出一个packet
            if (packet_queue_pop(&packet_queue_a, packet, 1) <= 0) {
                if (input_finish) {
                    av_packet_unref(packet);
                    packet = nullptr;
                    cout << "flushing decoder" << endl;
                } else {
                    av_packet_unref(packet);
                    return;
                }
            }

            //解码packet
            get_size = audio_decode_frame(c_ctx, packet, audio_buff, sizeof(audio_buff));
            if (get_size < 0) {
                //输出一段静音
                audio_len = 1024;
                memset(audio_buff, 0, audio_len);
                av_packet_unref(packet);
            } else if (get_size == 0) { //解码缓冲区被冲洗，解码完毕
                decode_finish = true;
            } else {
                audio_len = get_size;
                av_packet_unref(packet);
            }
            send_len = 0;

        }

        copy_len = audio_len - send_len;
        if (copy_len > len) {
            copy_len = len;
        }

        //todo 这里可不可以不强制转换
        memcpy(stream, (uint8_t *) audio_buff + send_len, copy_len);

        stream += copy_len;
        send_len += copy_len;
        len -= copy_len;
    }


}


int main(int argc, char *argv[]) {
    //ffmpeg
    AVFormatContext *format_ctx = nullptr;
    AVCodecContext *codec_ctx = nullptr;
    AVCodecParameters *codec_para = nullptr;
    AVCodec *codec = nullptr;
    AVPacket *packet = nullptr;
    int a_idx = -1;
    //sdl
    SDL_AudioSpec wanted_spec; //sdl设备支持的音频参数
    SDL_AudioSpec actual_spec;


    int ret = 0;
    if (argc < 2) {
        cout << "you should input file name" << endl;
        return ERR;
    }

    if (avformat_open_input(&format_ctx, argv[1], nullptr, nullptr) < 0) {
        cout << "open input error" << endl;
        return ERR;
    }
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        cout << "find stream error" << endl;
        return ERR;
    }
    av_dump_format(format_ctx, 0, argv[1], 0);

    for (int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            a_idx = i;
            break;
        }
    }
    if (a_idx == -1) {
        cout << "find audio stream error" << endl;
        return ERR;
    }

    //构建codec_context
    codec_para = format_ctx->streams[a_idx]->codecpar;
    codec = avcodec_find_decoder(codec_para->codec_id);
    if (codec == nullptr) {
        cout << "can not find codec" << endl;
        return ERR;
    }
    codec_ctx = avcodec_alloc_context3(codec);
    if (codec_ctx == nullptr) {
        cout << "avcode_alloc_context3 error" << endl;
        return ERR;
    }
    if (avcodec_parameters_to_context(codec_ctx, codec_para) < 0) {
        cout << "avcodec_parameters_to_context error" << endl;
        return ERR;
    }
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        cout << "avcodec_open2 error" << endl;
        return ERR;
    }


    packet = (AVPacket *) av_malloc(sizeof(AVPacket));


    if (packet == nullptr) {
        cout << "av_malloc error" << endl;
        return ERR;
    }

    //初始化sdl系统
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        cout << "init sdl error:" << SDL_GetError() << endl;
        return ERR;
    }

    packet_queue_init(&packet_queue_a);

    //打开音频设备并创建音频处理线程
    //设置参数，打开音频设备;如果设置了回调函数，sdl会以特定频率调用回调函数，在回调函数中获取音频数据
    wanted_spec.freq = codec_ctx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = codec_ctx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE; //SDL声音缓冲区大小
    wanted_spec.callback = sdl_audio_callback; //回调函数
    wanted_spec.userdata = codec_ctx; //回调函数的参数


    //按照wanted参数打开audio设备，实际的参数将会返回给actual_spec；SDL在单独的线程中实现对音频的处理
    if (SDL_OpenAudio(&wanted_spec, &actual_spec) != 0) {
        cout << "open audio error" << endl;
        return ERR;
    }
    //根据sdl音频参数构建音频重采样参数；因为解码之后的frame音频格式不一定能被sdl支持，因此需要frame重采样（转换格式）再送入sdl音频缓冲区
    //一下参数必须保证可以被sdl支持
    audio_para_tgt.freq = actual_spec.freq;
    audio_para_tgt.fmt = AV_SAMPLE_FMT_S16;
    audio_para_tgt.channels = actual_spec.channels;
    audio_para_tgt.channel_layout = av_get_default_channel_layout(actual_spec.channels);
    audio_para_tgt.frame_size = av_samples_get_buffer_size(nullptr, actual_spec.channels, 1, audio_para_tgt.fmt, 1);
    audio_para_tgt.bytes_per_sec = av_samples_get_buffer_size(nullptr, actual_spec.channels, audio_para_tgt.freq,
                                                              audio_para_tgt.fmt, 1);

    if (audio_para_tgt.bytes_per_sec <= 0 || audio_para_tgt.frame_size <= 0) {
        cout << "av_samples_get_buffer_size error" << endl;
        return ERR;
    }
    audio_para_src = audio_para_tgt;

    //dsl开始调用回调函数
    SDL_PauseAudio(0);

    while (av_read_frame(format_ctx, packet) == 0) {

        if (packet->stream_index == a_idx) {
            packet_queue_push(&packet_queue_a, packet);
        } else {
            av_packet_unref(packet);
        }
    }
    SDL_Delay(40);

    input_finish = true;
    while (!decode_finish) {
        SDL_Delay(1000);
    }

    SDL_Delay(1000);

    //todo exit



    return 0;
}
