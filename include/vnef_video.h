#ifndef VNEF_VIDEO_H
#define VNEF_VIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#if defined(_WIN32) && defined(VNEF_VIDEO_BUILD_DLL)
    #define VNEF_VIDEO_API __declspec(dllexport)
#elif defined(_WIN32)
    #define VNEF_VIDEO_API __declspec(dllimport)
#else
    #define VNEF_VIDEO_API
#endif

typedef struct VNEVideo VNEVideo;

typedef enum VNEFrameType {
    VNE_FRAME_NONE  = 0,
    VNE_FRAME_VIDEO = 1,
    VNE_FRAME_AUDIO = 2,
    VNE_FRAME_EOF   = 3,
    VNE_FRAME_ERROR = -1,
} VNEFrameType;

typedef struct VNEVideoInfo {
    int width;
    int height;
    int fps_num;
    int fps_den;
    int64_t duration_ms;
    int has_audio;
    int sample_rate;
    int channels;
} VNEVideoInfo;

typedef struct VNEVideoFrame {
    int width;
    int height;
    int stride;
    int64_t pts_ms;
    uint8_t *data;   // RGBA
} VNEVideoFrame;

typedef struct VNEAudioFrame {
    int sample_rate;
    int channels;
    int nb_samples;
    int bytes_per_sample; // 2 for S16
    int64_t pts_ms;
    uint8_t *data;         // interleaved S16
} VNEAudioFrame;

// Opens a media file or a custom .video container (header + raw WebM bytes).
VNEF_VIDEO_API VNEVideo *vne_video_open(const char *path, VNEVideoInfo *out_info);
VNEF_VIDEO_API void vne_video_close(VNEVideo *v);
VNEF_VIDEO_API const char *vne_video_last_error(VNEVideo *v);

// Returns which frame was produced. Use pts_ms to schedule playback.
VNEF_VIDEO_API VNEFrameType vne_video_next(VNEVideo *v, VNEVideoFrame *out_video, VNEAudioFrame *out_audio);

VNEF_VIDEO_API void vne_video_free_video_frame(VNEVideoFrame *f);
VNEF_VIDEO_API void vne_video_free_audio_frame(VNEAudioFrame *f);

// Seek to a timestamp in milliseconds. Returns 0 on success, -1 on failure.
VNEF_VIDEO_API int vne_video_seek_ms(VNEVideo *v, int64_t target_ms);

#ifdef __cplusplus
}
#endif

#endif // VNEF_VIDEO_H
