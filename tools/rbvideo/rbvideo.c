/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* librbvideo.so: FFmpeg and libwebp behind apps/video/rbvideo.h.
 *
 * One decoding thread per open stream, at the lowest priority the kernel
 * has (SCHED_IDLE, nice 19), with every signal blocked: Rockbox's own
 * threads are user-space contexts switched on its timer signal, and that
 * signal must never land here. */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#ifdef RBV_WEBP
#include <webp/demux.h>
#endif

#include "rbvideo.h"

#define EXPORT __attribute__((visibility("default")))

#define SEEK_JUMP_MS    1500    /* a forward jump bigger than this seeks */
#define LAG_SKIP_MS     250     /* skip non-reference frames past this */
#define LAG_SEEK_MS     3000    /* and seek past this */
#define PKT_QUEUE_MAX   512
#define AUDIO_RING_SEC  2

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------ packet queues */

struct pktq
{
    AVPacket *pkts[PKT_QUEUE_MAX];
    int head, count;
};

static int pktq_put(struct pktq *q, AVPacket *p)
{
    if (q->count >= PKT_QUEUE_MAX)
        return -1;
    q->pkts[(q->head + q->count) % PKT_QUEUE_MAX] = p;
    q->count++;
    return 0;
}

static AVPacket *pktq_get(struct pktq *q)
{
    AVPacket *p;
    if (!q->count)
        return NULL;
    p = q->pkts[q->head];
    q->head = (q->head + 1) % PKT_QUEUE_MAX;
    q->count--;
    return p;
}

static void pktq_flush(struct pktq *q)
{
    AVPacket *p;
    while ((p = pktq_get(q)))
        av_packet_free(&p);
}

/* ------------------------------------------------------------ stream */

struct rbv
{
    struct rbv_config cfg;
    char *path;

    /* demuxing and decoding - the thread's alone */
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vidx, aidx;
    struct pktq vq, aq;
    int demux_eof;
    AVFrame *frame;         /* decoded, waiting to be due */
    int have_pending;
    int64_t pending_pts;
    int64_t frame_dur;      /* ms, a guess until known */
    struct SwsContext *sws;
    struct SwrContext *swr;
    int video_eof;
    int skipping;
    int64_t duration;       /* ms; learnt at the first end when unknown */
    int64_t last_shown_pts;
    int64_t last_local;
#ifdef RBV_WEBP
    WebPAnimDecoder *webp;
    uint8_t *webp_file;
    int webp_prev_ts;
#endif

    /* shared with the caller, under lock */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int quit;
    int64_t clock_ms, clock_at;
    int playing;
    int status;
    uint16_t *front, *back;
    int serial, copied_serial;
    int64_t front_pts;
    int64_t last_get_at;
    int64_t last_present_at;
    struct rbv_stats stats;
    struct rbv_info info;

    /* audio ring */
    int16_t *ring;
    int ring_size, ring_head, ring_count;   /* in stereo frames */
    int64_t ring_pts;       /* time of the sample at ring_head */
};

/* ------------------------------------------------------------- open */

static AVCodecContext *open_decoder(AVStream *st, int fast)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *ctx;
    if (!codec)
        return NULL;
    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
        goto fail;
    /* One core: frame threads would only add copies and latency. */
    ctx->thread_count = 1;
    ctx->pkt_timebase = st->time_base;
    if (fast)
    {
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        ctx->skip_loop_filter = AVDISCARD_ALL;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0)
        goto fail;
    return ctx;
fail:
    avcodec_free_context(&ctx);
    return NULL;
}

static int is_webp(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ".webp");
}

#ifdef RBV_WEBP
static int webp_load(struct rbv *v, const char *path, struct rbv_info *info)
{
    FILE *f = fopen(path, "rb");
    long size;
    WebPData data;
    WebPAnimDecoderOptions opt;
    WebPAnimInfo ai;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 64 * 1024 * 1024)
    {
        fclose(f);
        return -1;
    }
    v->webp_file = malloc(size);
    if (!v->webp_file || fread(v->webp_file, 1, size, f) != (size_t)size)
    {
        fclose(f);
        return -1;
    }
    fclose(f);

    data.bytes = v->webp_file;
    data.size = size;
    if (!WebPAnimDecoderOptionsInit(&opt))
        return -1;
    opt.color_mode = MODE_RGBA;
    opt.use_threads = 0;
    v->webp = WebPAnimDecoderNew(&data, &opt);
    if (!v->webp || !WebPAnimDecoderGetInfo(v->webp, &ai))
        return -1;

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->width = ai.canvas_width;
    info->height = ai.canvas_height;
    info->has_video = 1;
    info->frames = ai.frame_count;
    snprintf(info->codec, sizeof(info->codec), "webp");

    /* The duration is the end of the last frame: walk it once. */
    {
        uint8_t *buf;
        int ts = 0;
        while (WebPAnimDecoderHasMoreFrames(v->webp))
            if (!WebPAnimDecoderGetNext(v->webp, &buf, &ts))
                break;
        info->duration_ms = ts;
        WebPAnimDecoderReset(v->webp);
    }
    if (info->frames > 0 && info->duration_ms > 0)
        info->fps_x1000 = (int)(info->frames * 1000000LL / info->duration_ms);
    return 0;
}
#endif

static int ff_load(struct rbv *v, const char *path, struct rbv_info *info,
                   int want_video, int want_audio)
{
    AVFormatContext *fmt = NULL;
    AVDictionary *opts = NULL;

    /* A GIF plays to its end; looping is ours to decide. */
    av_dict_set(&opts, "ignore_loop", "1", 0);
    if (avformat_open_input(&fmt, path, NULL, &opts) < 0)
    {
        av_dict_free(&opts);
        return -1;
    }
    av_dict_free(&opts);
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;
    if (avformat_find_stream_info(fmt, NULL) < 0)
    {
        avformat_close_input(&fmt);
        return -1;
    }

    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    v->vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    v->aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    /* Cover art in an audio file is a "video" stream of one picture. */
    if (v->vidx >= 0 &&
        (fmt->streams[v->vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        v->vidx = -1;

    if (v->vidx >= 0)
    {
        AVStream *st = fmt->streams[v->vidx];
        info->has_video = 1;
        info->width = st->codecpar->width;
        info->height = st->codecpar->height;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
            info->fps_x1000 = (int)(1000LL * st->avg_frame_rate.num /
                                    st->avg_frame_rate.den);
        info->frames = st->nb_frames > 0 ? (int)st->nb_frames : 0;
        snprintf(info->codec, sizeof(info->codec), "%s",
                 avcodec_get_name(st->codecpar->codec_id));
    }
    if (v->aidx >= 0)
    {
        info->has_audio = 1;
        if (v->vidx < 0)
            snprintf(info->codec, sizeof(info->codec), "%s",
                     avcodec_get_name(fmt->streams[v->aidx]->codecpar->codec_id));
    }
    if (fmt->duration > 0)
        info->duration_ms = fmt->duration / 1000;

    if (want_video && v->vidx >= 0)
    {
        v->vdec = open_decoder(fmt->streams[v->vidx],
                               v->cfg.flags & RBV_FAST);
        if (!v->vdec)
            v->vidx = -1;
    }
    else
        v->vidx = -1;
    if (want_audio && v->aidx >= 0)
    {
        v->adec = open_decoder(fmt->streams[v->aidx], 0);
        if (!v->adec)
            v->aidx = -1;
    }
    else
        v->aidx = -1;

    /* Only the streams we decode are worth reading. */
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if ((int)i != v->vidx && (int)i != v->aidx)
            fmt->streams[i]->discard = AVDISCARD_ALL;

    v->fmt = fmt;
    return 0;
}

static int rbv_probe(const char *path, struct rbv_info *info)
{
    struct rbv tmp;
    int ret;
    memset(&tmp, 0, sizeof(tmp));
#ifdef RBV_WEBP
    if (is_webp(path))
    {
        ret = webp_load(&tmp, path, info);
        if (tmp.webp)
            WebPAnimDecoderDelete(tmp.webp);
        free(tmp.webp_file);
        return ret;
    }
#endif
    ret = ff_load(&tmp, path, info, 0, 0);
    if (tmp.fmt)
        avformat_close_input(&tmp.fmt);
    return ret;
}

/* --------------------------------------------------------- decoding */

static int64_t to_ms(struct rbv *v, int idx, int64_t ts)
{
    AVStream *st = v->fmt->streams[idx];
    int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    if (ts == AV_NOPTS_VALUE)
        return AV_NOPTS_VALUE;
    return av_rescale_q(ts - start, st->time_base, (AVRational){1, 1000});
}

/* Reads one packet into the queue it belongs to. 0 at the end. */
static int demux_one(struct rbv *v)
{
    AVPacket *p;
    if (v->demux_eof)
        return 0;
    p = av_packet_alloc();
    if (!p)
        return -1;
    if (av_read_frame(v->fmt, p) < 0)
    {
        av_packet_free(&p);
        v->demux_eof = 1;
        return 0;
    }
    if (p->stream_index == v->vidx && !pktq_put(&v->vq, p))
        return 1;
    if (p->stream_index == v->aidx && !pktq_put(&v->aq, p))
        return 1;
    av_packet_free(&p);
    return 1;
}

static void audio_ring_write(struct rbv *v, const int16_t *s, int frames,
                             int64_t pts)
{
    pthread_mutex_lock(&v->lock);
    if (v->ring_count == 0 && pts != AV_NOPTS_VALUE)
        v->ring_pts = pts;
    while (frames > 0 && v->ring_count < v->ring_size)
    {
        int tail = (v->ring_head + v->ring_count) % v->ring_size;
        int n = v->ring_size - tail;
        if (n > v->ring_size - v->ring_count)
            n = v->ring_size - v->ring_count;
        if (n > frames)
            n = frames;
        memcpy(v->ring + 2 * tail, s, 2 * n * sizeof(int16_t));
        v->ring_count += n;
        s += 2 * n;
        frames -= n;
    }
    pthread_mutex_unlock(&v->lock);
}

static int audio_ring_space(struct rbv *v)
{
    int space;
    pthread_mutex_lock(&v->lock);
    space = v->ring_size - v->ring_count;
    pthread_mutex_unlock(&v->lock);
    return space;
}

/* Decodes queued audio while the ring has room for it. */
static void decode_audio(struct rbv *v)
{
    AVFrame *f;
    AVPacket *p;
    int16_t *out = NULL;
    int out_cap = 0;

    if (!v->adec)
        return;
    f = av_frame_alloc();
    if (!f)
        return;
    while (audio_ring_space(v) > v->cfg.audio_rate / 4)
    {
        int r = avcodec_receive_frame(v->adec, f);
        if (r == AVERROR(EAGAIN))
        {
            p = pktq_get(&v->aq);
            if (!p)
            {
                if (v->demux_eof || v->vq.count >= PKT_QUEUE_MAX - 1)
                    break;
                if (demux_one(v) <= 0)
                    break;
                continue;
            }
            avcodec_send_packet(v->adec, p);
            av_packet_free(&p);
            continue;
        }
        if (r < 0)
            break;

        if (!v->swr)
        {
            AVChannelLayout out_layout;
            av_channel_layout_default(&out_layout, 2);
            if (swr_alloc_set_opts2(&v->swr, &out_layout, AV_SAMPLE_FMT_S16,
                                    v->cfg.audio_rate, &f->ch_layout,
                                    f->format, f->sample_rate, 0, NULL) < 0 ||
                swr_init(v->swr) < 0)
            {
                swr_free(&v->swr);
                break;
            }
        }
        {
            int max = (int)av_rescale_rnd(swr_get_delay(v->swr, f->sample_rate)
                                          + f->nb_samples, v->cfg.audio_rate,
                                          f->sample_rate, AV_ROUND_UP);
            if (max > out_cap)
            {
                free(out);
                out_cap = max;
                out = malloc(2 * sizeof(int16_t) * out_cap);
                if (!out)
                    break;
            }
            uint8_t *dst = (uint8_t *)out;
            int n = swr_convert(v->swr, &dst, max,
                                (const uint8_t **)f->extended_data,
                                f->nb_samples);
            if (n > 0)
                audio_ring_write(v, out, n,
                                 to_ms(v, v->aidx, f->best_effort_timestamp));
        }
        av_frame_unref(f);
    }
    free(out);
    av_frame_free(&f);
}

/* The next decoded video frame, into v->frame. 0 at the end. */
static int decode_video(struct rbv *v)
{
#ifdef RBV_WEBP
    if (v->webp)
    {
        uint8_t *buf;
        int ts;
        if (!WebPAnimDecoderHasMoreFrames(v->webp) ||
            !WebPAnimDecoderGetNext(v->webp, &buf, &ts))
            return 0;
        AVFrame *f = v->frame;
        av_frame_unref(f);
        f->format = AV_PIX_FMT_RGBA;
        f->width = v->info.width;
        f->height = v->info.height;
        f->data[0] = buf;
        f->linesize[0] = 4 * v->info.width;
        v->pending_pts = v->webp_prev_ts;
        v->frame_dur = ts - v->webp_prev_ts;
        v->webp_prev_ts = ts;
        v->stats.decoded++;
        return 1;
    }
#endif
    for (;;)
    {
        int r = avcodec_receive_frame(v->vdec, v->frame);
        if (r == 0)
        {
            int64_t pts = to_ms(v, v->vidx, v->frame->best_effort_timestamp);
            if (pts == AV_NOPTS_VALUE)
                pts = v->pending_pts + v->frame_dur;
            if (v->frame->duration > 0)
                v->frame_dur = av_rescale_q(v->frame->duration,
                    v->fmt->streams[v->vidx]->time_base, (AVRational){1, 1000});
            v->pending_pts = pts;
            v->stats.decoded++;
            return 1;
        }
        if (r == AVERROR_EOF)
            return 0;
        if (r != AVERROR(EAGAIN))
            return -1;

        AVPacket *p = pktq_get(&v->vq);
        while (!p)
        {
            int d = demux_one(v);
            if (d < 0)
                return -1;
            if (d == 0)
                break;
            /* Keep audio decoding while video waits for its packets. */
            if (v->aq.count >= PKT_QUEUE_MAX - 1)
                decode_audio(v);
            p = pktq_get(&v->vq);
        }
        avcodec_send_packet(v->vdec, p);   /* NULL drains at the end */
        av_packet_free(&p);
    }
}

static void do_seek(struct rbv *v, int64_t ms)
{
#ifdef RBV_WEBP
    if (v->webp)
    {
        WebPAnimDecoderReset(v->webp);
        v->webp_prev_ts = 0;
        v->have_pending = 0;
        v->video_eof = 0;
        return;
    }
#endif
    int idx = v->vidx >= 0 ? v->vidx : v->aidx;
    AVStream *st = v->fmt->streams[idx];
    int64_t ts = av_rescale_q(ms, (AVRational){1, 1000}, st->time_base);
    if (st->start_time != AV_NOPTS_VALUE)
        ts += st->start_time;
    av_seek_frame(v->fmt, idx, ts, AVSEEK_FLAG_BACKWARD);
    pktq_flush(&v->vq);
    pktq_flush(&v->aq);
    if (v->vdec)
        avcodec_flush_buffers(v->vdec);
    if (v->adec)
        avcodec_flush_buffers(v->adec);
    if (v->swr)
        swr_free(&v->swr);
    pthread_mutex_lock(&v->lock);
    v->ring_count = 0;
    v->ring_pts = ms;
    pthread_mutex_unlock(&v->lock);
    v->demux_eof = 0;
    v->video_eof = 0;
    v->have_pending = 0;
}

/* Scales v->frame into the back buffer and swaps it to the front. */
static void present(struct rbv *v)
{
    AVFrame *f = v->frame;
    int ow = v->cfg.out_w, oh = v->cfg.out_h;
    int sx = 0, sy = 0, sw = f->width, sh = f->height;
    int dx = 0, dy = 0, dw = ow, dh = oh;
    const uint8_t *src[4];
    int srcstride[4];
    uint8_t *dst[4] = { NULL };
    int dststride[4] = { 0 };

    if (sw <= 0 || sh <= 0)
        return;
    if (v->cfg.fit == RBV_FIT_COVER)
    {
        /* Crop the source to the output's shape. */
        if ((int64_t)sw * oh > (int64_t)sh * ow)
        {
            int nw = (int)((int64_t)sh * ow / oh);
            sx = (sw - nw) / 2;
            sw = nw;
        }
        else
        {
            int nh = (int)((int64_t)sw * oh / ow);
            sy = (sh - nh) / 2;
            sh = nh;
        }
    }
    else if (v->cfg.fit == RBV_FIT_CONTAIN)
    {
        if ((int64_t)sw * oh > (int64_t)sh * ow)
            dh = (int)((int64_t)sh * ow / sw);
        else
            dw = (int)((int64_t)sw * oh / sh);
        dw = dw < 1 ? 1 : dw;
        dh = dh < 1 ? 1 : dh;
        dx = (ow - dw) / 2;
        dy = (oh - dh) / 2;
    }

    /* Chroma is subsampled: keep the crop even. */
    sx &= ~1;
    sy &= ~1;
    if (sx || sy || sw != f->width || sh != f->height)
    {
        f->crop_left = sx;
        f->crop_top = sy;
        f->crop_right = f->width - sx - sw;
        f->crop_bottom = f->height - sy - sh;
        if (av_frame_apply_cropping(f, AV_FRAME_CROP_UNALIGNED) < 0)
            return;
        sw = f->width;
        sh = f->height;
    }
    for (int i = 0; i < 4; i++)
    {
        src[i] = f->data[i];
        srcstride[i] = f->linesize[i];
    }

    v->sws = sws_getCachedContext(v->sws, sw, sh, f->format, dw, dh,
                                  AV_PIX_FMT_RGB565,
                                  (v->cfg.flags & RBV_FAST) ? SWS_POINT
                                                            : SWS_FAST_BILINEAR,
                                  NULL, NULL, NULL);
    if (!v->sws)
        return;

    if (v->cfg.fit == RBV_FIT_CONTAIN && (dw != ow || dh != oh))
        memset(v->back, 0, sizeof(uint16_t) * ow * oh);
    dst[0] = (uint8_t *)(v->back + dy * ow + dx);
    dststride[0] = ow * 2;
    sws_scale(v->sws, src, srcstride, 0, sh, dst, dststride);

    pthread_mutex_lock(&v->lock);
    uint16_t *t = v->front;
    v->front = v->back;
    v->back = t;
    v->front_pts = v->pending_pts;
    v->serial++;
    v->last_present_at = now_ms();
    v->stats.shown++;
    pthread_mutex_unlock(&v->lock);
    v->last_shown_pts = v->pending_pts;
}

static void set_skipping(struct rbv *v, int on)
{
    if (!v->vdec || on == v->skipping)
        return;
    v->skipping = on;
    v->vdec->skip_frame = on ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    if (!(v->cfg.flags & RBV_FAST))
        v->vdec->skip_loop_filter = on ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
}

static void *thread_main(void *arg)
{
    struct rbv *v = arg;
    int has_video = v->vdec != NULL;
#ifdef RBV_WEBP
    has_video = has_video || v->webp != NULL;
#endif

    /* Last in line for the processor - unless this is the player, whose
     * clip is the one thing on screen and the only sound. SCHED_IDLE gets
     * nothing at all while anything else can run, and the R1 has one core:
     * the player got no sound and no pictures, only its progress bar. */
    if (!(v->cfg.flags & RBV_AUDIO))
    {
        struct sched_param sp = { 0 };
        pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    }

    v->last_local = -1;
    while (1)
    {
        int64_t clock_ms, clock_at, last_get, now;
        int playing;

        pthread_mutex_lock(&v->lock);
        if (v->quit)
        {
            pthread_mutex_unlock(&v->lock);
            break;
        }
        clock_ms = v->clock_ms;
        clock_at = v->clock_at;
        playing = v->playing;
        last_get = v->last_get_at;
        pthread_mutex_unlock(&v->lock);

        now = now_ms();
        if (v->cfg.idle_ms && now - last_get > v->cfg.idle_ms &&
            v->stats.shown > 0)
        {
            /* Nobody is looking: no work. */
            sleep_ms(50);
            continue;
        }

        int64_t target = clock_ms + (playing ? now - clock_at : 0);
        if (target < 0)
            target = 0;
        if ((v->cfg.flags & RBV_LOOP) && v->duration > 0)
            target %= v->duration;

        /* Audio first: it is what the listener hears. */
        if (v->adec)
            decode_audio(v);

        if (!has_video)
        {
            if (v->demux_eof && !v->vq.count && !v->aq.count)
            {
                pthread_mutex_lock(&v->lock);
                if (v->ring_count == 0)
                    v->status = RBV_EOF;
                pthread_mutex_unlock(&v->lock);
            }
            sleep_ms(10);
            continue;
        }

        /* Wrapped round, or someone moved the clock back, or far ahead. */
        if (v->last_local >= 0 &&
            (target < v->last_local - 200 ||
             (target > v->last_local + SEEK_JUMP_MS &&
              (!v->have_pending || target > v->pending_pts + SEEK_JUMP_MS))))
        {
            do_seek(v, target);
            pthread_mutex_lock(&v->lock);
            if (v->status == RBV_EOF)
                v->status = RBV_OK;
            pthread_mutex_unlock(&v->lock);
        }
        v->last_local = target;

        if (!v->have_pending && !v->video_eof)
        {
            int r = decode_video(v);
            if (r < 0)
            {
                pthread_mutex_lock(&v->lock);
                v->status = RBV_ERROR;
                pthread_mutex_unlock(&v->lock);
                sleep_ms(50);
                continue;
            }
            if (r == 0)
            {
                v->video_eof = 1;
                if (v->duration <= 0)
                    v->duration = v->last_shown_pts + (v->frame_dur > 0 ?
                                                       v->frame_dur : 40);
                if (v->cfg.flags & RBV_LOOP)
                {
                    /* Wait for the clock to come round. */
                    if (v->duration > 0)
                        continue;
                }
                pthread_mutex_lock(&v->lock);
                v->status = RBV_EOF;
                pthread_mutex_unlock(&v->lock);
                continue;
            }
            v->have_pending = 1;
        }

        if (v->video_eof)
        {
            sleep_ms(20);
            continue;
        }

        /* Is the frame due? Is the next one due too (then skip this)? */
        int64_t lag = target - v->pending_pts;
        if (lag >= 0 || v->stats.shown == 0)
        {
            if (lag > LAG_SEEK_MS && v->stats.shown > 0)
            {
                do_seek(v, target);
                continue;
            }
            set_skipping(v, lag > LAG_SKIP_MS);
            if (lag > v->frame_dur && v->stats.shown > 0)
            {
                /* A later frame is due already: drop this one unseen. */
                v->stats.dropped++;
                v->have_pending = 0;
                continue;
            }
            if (v->cfg.max_fps > 0 && v->stats.shown > 0 &&
                now - v->last_present_at < 1000 / v->cfg.max_fps)
            {
                sleep_ms(1000 / v->cfg.max_fps - (int)(now - v->last_present_at));
                continue;
            }
            v->stats.lag_ms = (int)(lag > 0 ? lag : 0);
            present(v);
            v->have_pending = 0;
            continue;
        }

        /* Not due yet: wait for it, or for the clock to change. */
        {
            int wait = (int)(-lag);
            struct timespec ts;
            if (wait > 20 || !playing)
                wait = 20;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += wait * 1000000L;
            if (ts.tv_nsec >= 1000000000L)
            {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_mutex_lock(&v->lock);
            if (!v->quit)
                pthread_cond_timedwait(&v->wake, &v->lock, &ts);
            pthread_mutex_unlock(&v->lock);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ API */

static void rbv_close(struct rbv *v);

static struct rbv *rbv_open(const char *path, const struct rbv_config *cfg)
{
    struct rbv *v;
    int want_video, want_audio;
    sigset_t all, old;

    if (!path || !cfg || cfg->size < (int)sizeof(struct rbv_config))
        return NULL;
    v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->cfg = *cfg;
    v->vidx = v->aidx = -1;
    v->frame_dur = 40;
    v->path = strdup(path);
    pthread_mutex_init(&v->lock, NULL);
    pthread_cond_init(&v->wake, NULL);
    v->frame = av_frame_alloc();
    if (!v->frame)
        goto fail;

    want_video = !(cfg->flags & RBV_NO_VIDEO) && cfg->out_w > 0 &&
                 cfg->out_h > 0;
    want_audio = (cfg->flags & RBV_AUDIO) && cfg->audio_rate > 0;

#ifdef RBV_WEBP
    if (is_webp(path))
    {
        if (!want_video || webp_load(v, path, &v->info) < 0)
            goto fail;
        v->duration = v->info.duration_ms;
    }
    else
#endif
    {
        if (ff_load(v, path, &v->info, want_video, want_audio) < 0)
            goto fail;
        v->duration = v->info.duration_ms;
        if (!v->vdec && !v->adec)
            goto fail;
    }

    if (want_video)
    {
        size_t n = (size_t)cfg->out_w * cfg->out_h * sizeof(uint16_t);
        v->front = calloc(1, n);
        v->back = calloc(1, n);
        if (!v->front || !v->back)
            goto fail;
    }
    if (want_audio && v->adec)
    {
        v->ring_size = cfg->audio_rate * AUDIO_RING_SEC;
        v->ring = malloc(2 * sizeof(int16_t) * v->ring_size);
        if (!v->ring)
            goto fail;
    }

    v->clock_at = now_ms();
    v->last_get_at = v->clock_at;

    /* The thread inherits this mask: no Rockbox signal is ever ours. */
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    int r = pthread_create(&v->thread, NULL, thread_main, v);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r != 0)
        goto fail;
    return v;

fail:
    v->thread = 0;
    rbv_close(v);
    return NULL;
}

static void rbv_close(struct rbv *v)
{
    if (!v)
        return;
    if (v->thread)
    {
        pthread_mutex_lock(&v->lock);
        v->quit = 1;
        pthread_cond_broadcast(&v->wake);
        pthread_mutex_unlock(&v->lock);
        pthread_join(v->thread, NULL);
    }
    pktq_flush(&v->vq);
    pktq_flush(&v->aq);
    avcodec_free_context(&v->vdec);
    avcodec_free_context(&v->adec);
    if (v->fmt)
        avformat_close_input(&v->fmt);
    sws_freeContext(v->sws);
    swr_free(&v->swr);
    if (v->frame)
    {
#ifdef RBV_WEBP
        if (v->webp)
        {
            /* Its data is libwebp's. */
            v->frame->data[0] = NULL;
            v->frame->buf[0] = NULL;
        }
#endif
        av_frame_free(&v->frame);
    }
#ifdef RBV_WEBP
    if (v->webp)
        WebPAnimDecoderDelete(v->webp);
    free(v->webp_file);
#endif
    free(v->front);
    free(v->back);
    free(v->ring);
    free(v->path);
    pthread_mutex_destroy(&v->lock);
    pthread_cond_destroy(&v->wake);
    free(v);
}

static void rbv_set_clock(struct rbv *v, int64_t media_ms, int playing)
{
    pthread_mutex_lock(&v->lock);
    int64_t now = now_ms();
    int64_t was = v->clock_ms + (v->playing ? now - v->clock_at : 0);
    v->clock_ms = media_ms;
    v->clock_at = now;
    if (playing != v->playing || media_ms < was - 50 || media_ms > was + 50)
        pthread_cond_broadcast(&v->wake);
    v->playing = playing;
    pthread_mutex_unlock(&v->lock);
}

static int rbv_get_frame(struct rbv *v, uint16_t *dst, int stride,
                         int64_t *pts)
{
    int ret = 0;
    pthread_mutex_lock(&v->lock);
    v->last_get_at = now_ms();
    if (v->status == RBV_ERROR && v->serial == 0)
        ret = RBV_ERROR;
    else if (v->front && v->serial != v->copied_serial)
    {
        int w = v->cfg.out_w, h = v->cfg.out_h;
        for (int y = 0; y < h; y++)
            memcpy(dst + y * stride, v->front + y * w, w * sizeof(uint16_t));
        v->copied_serial = v->serial;
        if (pts)
            *pts = v->front_pts;
        ret = 1;
    }
    pthread_mutex_unlock(&v->lock);
    return ret;
}

static int rbv_read_audio(struct rbv *v, int16_t *buf, int frames,
                          int64_t *pts)
{
    int done = 0;
    pthread_mutex_lock(&v->lock);
    if (pts)
        *pts = v->ring_pts;
    while (done < frames && v->ring_count > 0)
    {
        int n = v->ring_size - v->ring_head;
        if (n > v->ring_count)
            n = v->ring_count;
        if (n > frames - done)
            n = frames - done;
        memcpy(buf + 2 * done, v->ring + 2 * v->ring_head,
               2 * n * sizeof(int16_t));
        v->ring_head = (v->ring_head + n) % v->ring_size;
        v->ring_count -= n;
        done += n;
    }
    if (v->cfg.audio_rate > 0)
        v->ring_pts += (int64_t)done * 1000 / v->cfg.audio_rate;
    pthread_mutex_unlock(&v->lock);
    return done;
}

static int rbv_status(struct rbv *v)
{
    int s;
    pthread_mutex_lock(&v->lock);
    s = v->status;
    pthread_mutex_unlock(&v->lock);
    return s;
}

static void rbv_stats(struct rbv *v, struct rbv_stats *st)
{
    pthread_mutex_lock(&v->lock);
    *st = v->stats;
    st->size = sizeof(*st);
    pthread_mutex_unlock(&v->lock);
}

static int rbv_info(struct rbv *v, struct rbv_info *info)
{
    *info = v->info;
    if (v->duration > info->duration_ms)
        info->duration_ms = v->duration;
    return 0;
}

static int rbv_lock_frame(struct rbv *v, const uint16_t **px, int64_t *pts)
{
    pthread_mutex_lock(&v->lock);
    v->last_get_at = now_ms();
    *px = v->front;
    if (pts)
        *pts = v->front_pts;
    if (v->serial == 0)
        return v->status == RBV_ERROR ? RBV_ERROR : 0;
    return v->serial;
}

static void rbv_unlock_frame(struct rbv *v)
{
    pthread_mutex_unlock(&v->lock);
}

/* Named __header so Rockbox's own code loader (lc_open, lc_get_header)
 * can find it on the device and in the simulator alike. */
EXPORT const struct rbv_api __header =
{
    .version = RBV_API_VERSION,
    .build = "librbvideo " LIBAVCODEC_IDENT
#ifdef RBV_WEBP
             ", libwebp"
#endif
    ,
    .probe = rbv_probe,
    .open = rbv_open,
    .close = rbv_close,
    .set_clock = rbv_set_clock,
    .get_frame = rbv_get_frame,
    .read_audio = rbv_read_audio,
    .status = rbv_status,
    .stats = rbv_stats,
    .info = rbv_info,
    .lock_frame = rbv_lock_frame,
    .unlock_frame = rbv_unlock_frame,
};
