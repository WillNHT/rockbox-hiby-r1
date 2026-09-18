/* rbvtest: exercise librbvideo.so outside Rockbox.
 *
 *   cc -I apps/video -o rbvtest tools/rbvideo/rbvtest.c -ldl
 *   ./rbvtest ./librbvideo.so clip.mp4 [out_w out_h fit loop ms_per_step steps]
 *
 * Probes the file, plays it on a simulated clock and writes every new
 * frame it is handed as frameNNN.ppm, then prints the decoder's counters.
 * With RBVTEST_AUDIO=1 it also drains the audio and reports how much
 * came out. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "rbvideo.h"

static void write_ppm(const char *name, const uint16_t *px, int w, int h)
{
    FILE *f = fopen(name, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++)
    {
        unsigned p = px[i];
        unsigned char rgb[3] = { (p >> 11) << 3, ((p >> 5) & 63) << 2,
                                 (p & 31) << 3 };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s lib file [w h fit loop step_ms steps]\n",
                argv[0]);
        return 1;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h)
    {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    const struct rbv_api *api = dlsym(h, "__header");
    if (!api || api->version != RBV_API_VERSION)
    {
        fprintf(stderr, "bad api\n");
        return 1;
    }
    printf("build: %s\n", api->build);

    struct rbv_info info = { sizeof(info) };
    if (api->probe(argv[2], &info) < 0)
    {
        fprintf(stderr, "probe failed\n");
        return 1;
    }
    printf("probe: %dx%d %lld ms fps %d.%03d video %d audio %d frames %d codec %s\n",
           info.width, info.height, (long long)info.duration_ms,
           info.fps_x1000 / 1000, info.fps_x1000 % 1000, info.has_video,
           info.has_audio, info.frames, info.codec);

    struct rbv_config cfg = { sizeof(cfg) };
    cfg.out_w = argc > 3 ? atoi(argv[3]) : 240;
    cfg.out_h = argc > 4 ? atoi(argv[4]) : 240;
    cfg.fit = argc > 5 ? atoi(argv[5]) : RBV_FIT_COVER;
    cfg.flags = (argc > 6 && atoi(argv[6])) ? RBV_LOOP : 0;
    int step = argc > 7 ? atoi(argv[7]) : 40;
    int steps = argc > 8 ? atoi(argv[8]) : 100;
    int audio = getenv("RBVTEST_AUDIO") != NULL;
    if (audio)
    {
        cfg.flags |= RBV_AUDIO;
        cfg.audio_rate = 44100;
    }

    struct rbv *v = api->open(argv[2], &cfg);
    if (!v)
    {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    uint16_t *buf = calloc(cfg.out_w * cfg.out_h, sizeof(uint16_t));
    int16_t abuf[4096 * 2];
    long audio_frames = 0;
    int n = 0;
    for (int i = 0; i < steps; i++)
    {
        api->set_clock(v, (int64_t)i * step, 1);
        struct timespec ts = { 0, step * 1000000L };
        nanosleep(&ts, NULL);
        int64_t pts;
        int r = api->get_frame(v, buf, cfg.out_w, &pts);
        if (r < 0)
        {
            printf("error at step %d\n", i);
            break;
        }
        if (r == 1)
        {
            char name[64];
            snprintf(name, sizeof(name), "frame%03d.ppm", n++);
            if (n <= 8 || n % 25 == 0)
                write_ppm(name, buf, cfg.out_w, cfg.out_h);
            printf("t=%d frame pts=%lld\n", i * step, (long long)pts);
        }
        if (audio)
        {
            int64_t apts;
            int got;
            while ((got = api->read_audio(v, abuf, 4096, &apts)) > 0)
                audio_frames += got;
        }
    }
    struct rbv_stats st = { sizeof(st) };
    api->stats(v, &st);
    printf("stats: decoded %d shown %d dropped %d lag %d status %d frames_copied %d audio %ld\n",
           st.decoded, st.shown, st.dropped, st.lag_ms, api->status(v), n,
           audio_frames);
    api->close(v);
    return 0;
}
