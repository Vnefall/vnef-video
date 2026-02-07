#include "vnef_video.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: %s <video-file>\n", argv[0]);
        return 1;
    }

    VNEVideoInfo info;
    VNEVideo *v = vne_video_open(argv[1], &info);
    if (!v) {
        printf("Failed to open video.\n");
        return 1;
    }

    printf("Video: %dx%d fps=%d/%d duration=%lldms\n",
        info.width, info.height, info.fps_num, info.fps_den, (long long)info.duration_ms);
    if (info.has_audio) {
        printf("Audio: %d Hz, %d channels\n", info.sample_rate, info.channels);
    } else {
        printf("Audio: none\n");
    }

    int vcount = 0;
    int acount = 0;
    while (vcount < 3 || acount < 3) {
        VNEVideoFrame vf = {0};
        VNEAudioFrame af = {0};
        VNEFrameType t = vne_video_next(v, &vf, &af);
        if (t == VNE_FRAME_VIDEO) {
            printf("Video frame %d pts=%lldms\n", vcount, (long long)vf.pts_ms);
            vne_video_free_video_frame(&vf);
            vcount++;
        } else if (t == VNE_FRAME_AUDIO) {
            printf("Audio frame %d pts=%lldms samples=%d\n", acount, (long long)af.pts_ms, af.nb_samples);
            vne_video_free_audio_frame(&af);
            acount++;
        } else if (t == VNE_FRAME_EOF) {
            printf("EOF\n");
            break;
        } else if (t == VNE_FRAME_ERROR) {
            printf("Error: %s\n", vne_video_last_error(v));
            break;
        }
    }

    vne_video_close(v);
    return 0;
}
