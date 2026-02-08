#include "vnef_video.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#if !defined(_WIN32)
#include <sys/types.h>
#endif

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#ifndef VNEF_VIDEO_DEBUG
#define VNEF_VIDEO_DEBUG 0
#endif

#if VNEF_VIDEO_DEBUG
#define VNEF_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define VNEF_LOG(...) ((void)0)
#endif

struct VNEVideo {
    AVFormatContext *fmt;
    AVCodecContext *vdec;
    AVCodecContext *adec;
    AVStream *vstream;
    AVStream *astream;
    int vstream_index;
    int astream_index;
    AVFrame *vframe;
    AVFrame *aframe;
    AVPacket *pkt;
    struct SwsContext *sws;
    struct SwrContext *swr;
    int sws_w;
    int sws_h;
    enum AVPixelFormat sws_fmt;
    enum AVSampleFormat out_sample_fmt;
    AVIOContext *avio;
    struct VNEVideoIO *io;
    int eof;
    char last_error[256];
};

typedef struct VNEVideoIO {
    FILE *fp;
    int64_t data_offset;
    int64_t data_size;
    int64_t pos;
} VNEVideoIO;

static int64_t vne_file_tell(FILE *fp) {
#if defined(_WIN32)
    return _ftelli64(fp);
#else
    return ftello(fp);
#endif
}

static int vne_file_seek(FILE *fp, int64_t offset, int whence) {
#if defined(_WIN32)
    return _fseeki64(fp, offset, whence);
#else
    return fseeko(fp, (off_t)offset, whence);
#endif
}

static int64_t vne_file_size(FILE *fp) {
    int64_t cur = vne_file_tell(fp);
    if (cur < 0) return -1;
    if (vne_file_seek(fp, 0, SEEK_END) != 0) return -1;
    int64_t size = vne_file_tell(fp);
    vne_file_seek(fp, cur, SEEK_SET);
    return size;
}

static int vne_video_probe_header(FILE *fp, int64_t *out_size) {
    uint8_t hdr[16];

    if (vne_file_seek(fp, 0, SEEK_SET) != 0) {
        return -1;
    }

    size_t n = fread(hdr, 1, sizeof(hdr), fp);
    if (n != sizeof(hdr)) {
        return 0; // too small or not a .video container
    }

    if (memcmp(hdr, "VID0", 4) != 0) {
        return 0; // not our container
    }

    uint32_t version = (uint32_t)hdr[4]
        | ((uint32_t)hdr[5] << 8)
        | ((uint32_t)hdr[6] << 16)
        | ((uint32_t)hdr[7] << 24);

    if (version != 1) {
        return -1; // unsupported version
    }

    uint64_t size = (uint64_t)hdr[8]
        | ((uint64_t)hdr[9] << 8)
        | ((uint64_t)hdr[10] << 16)
        | ((uint64_t)hdr[11] << 24)
        | ((uint64_t)hdr[12] << 32)
        | ((uint64_t)hdr[13] << 40)
        | ((uint64_t)hdr[14] << 48)
        | ((uint64_t)hdr[15] << 56);

    int64_t total = vne_file_size(fp);
    if (total < 0 || total < 16) {
        return -1;
    }

    if (size == 0) {
        size = (uint64_t)(total - 16);
    }

    if (size > (uint64_t)(total - 16)) {
        return -1;
    }

    *out_size = (int64_t)size;
    return 1;
}

static int vne_video_read(void *opaque, uint8_t *buf, int buf_size) {
    VNEVideoIO *io = (VNEVideoIO *)opaque;
    if (!io || !io->fp) return AVERROR_EOF;

    int64_t remaining = io->data_size - io->pos;
    if (remaining <= 0) return AVERROR_EOF;

    int to_read = buf_size;
    if ((int64_t)to_read > remaining) {
        to_read = (int)remaining;
    }

    int64_t target = io->data_offset + io->pos;
    if (vne_file_seek(io->fp, target, SEEK_SET) != 0) {
        return AVERROR(EIO);
    }

    size_t got = fread(buf, 1, (size_t)to_read, io->fp);
    if (got == 0) {
        return AVERROR_EOF;
    }

    io->pos += (int64_t)got;
    return (int)got;
}

static int64_t vne_video_seek(void *opaque, int64_t offset, int whence) {
    VNEVideoIO *io = (VNEVideoIO *)opaque;
    if (!io || !io->fp) return -1;

    if (whence == AVSEEK_SIZE) {
        return io->data_size;
    }

    int64_t new_pos = 0;
    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos = io->pos + offset;
    } else if (whence == SEEK_END) {
        new_pos = io->data_size + offset;
    } else {
        return -1;
    }

    if (new_pos < 0) return -1;
    if (new_pos > io->data_size) new_pos = io->data_size;

    io->pos = new_pos;
    return io->pos;
}

static void set_error(VNEVideo *v, const char *msg) {
    if (!v) return;
    snprintf(v->last_error, sizeof(v->last_error), "%s", msg ? msg : "unknown error");
}

static void set_ff_error(VNEVideo *v, int err, const char *context) {
    char buf[128];
    av_strerror(err, buf, sizeof(buf));
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: %s", context, buf);
    set_error(v, msg);
}

static int64_t pts_to_ms(AVStream *st, int64_t pts) {
    if (!st || pts == AV_NOPTS_VALUE) return -1;
    AVRational tb = st->time_base;
    return av_rescale_q(pts, tb, (AVRational){1, 1000});
}

static int init_video_decoder(VNEVideo *v) {
    int idx = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (idx < 0) {
        set_error(v, "no video stream found");
        return -1;
    }
    v->vstream_index = idx;
    v->vstream = v->fmt->streams[idx];

    AVCodecParameters *par = v->vstream->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        set_error(v, "video decoder not found");
        return -1;
    }

    v->vdec = avcodec_alloc_context3(codec);
    if (!v->vdec) {
        set_error(v, "failed to alloc video codec context");
        return -1;
    }

    if (avcodec_parameters_to_context(v->vdec, par) < 0) {
        set_error(v, "failed to copy video codec parameters");
        return -1;
    }

    if (avcodec_open2(v->vdec, codec, NULL) < 0) {
        set_error(v, "failed to open video decoder");
        return -1;
    }

    v->sws = sws_getContext(
        v->vdec->width,
        v->vdec->height,
        v->vdec->pix_fmt,
        v->vdec->width,
        v->vdec->height,
        AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );
    if (!v->sws) {
        set_error(v, "failed to create sws context");
        return -1;
    }
    v->sws_w = v->vdec->width;
    v->sws_h = v->vdec->height;
    v->sws_fmt = v->vdec->pix_fmt;

    return 0;
}

static int init_audio_decoder(VNEVideo *v) {
    int idx = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (idx < 0) {
        v->astream_index = -1;
        v->astream = NULL;
        return 0; // audio is optional
    }

    v->astream_index = idx;
    v->astream = v->fmt->streams[idx];

    AVCodecParameters *par = v->astream->codecpar;
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        char msg[256];
        snprintf(msg, sizeof(msg), "audio decoder not found for codec_id %d", par->codec_id);
        set_error(v, msg);
        return -1;
    }

    v->adec = avcodec_alloc_context3(codec);
    if (!v->adec) {
        set_error(v, "failed to alloc audio codec context (out of memory)");
        return -1;
    }

    int ret = avcodec_parameters_to_context(v->adec, par);
    if (ret < 0) {
        set_ff_error(v, ret, "failed to copy audio codec parameters");
        return -1;
    }

    ret = avcodec_open2(v->adec, codec, NULL);
    if (ret < 0) {
        set_ff_error(v, ret, "failed to open audio decoder");
        return -1;
    }

    AVChannelLayout in_layout = {0};
    int ch = v->adec->ch_layout.nb_channels > 0 ? v->adec->ch_layout.nb_channels : (v->adec->channels > 0 ? v->adec->channels : 2);
    av_channel_layout_default(&in_layout, ch);

    AVChannelLayout out_layout = {0};
    av_channel_layout_default(&out_layout, ch);

    ret = swr_alloc_set_opts2(
        &v->swr,
        &out_layout,
        AV_SAMPLE_FMT_S16,
        v->adec->sample_rate,
        &in_layout,
        v->adec->sample_fmt,
        v->adec->sample_rate,
        0,
        NULL
    );
    
    av_channel_layout_uninit(&out_layout);
    av_channel_layout_uninit(&in_layout);

    if (ret < 0) {
        set_ff_error(v, ret, "failed to allocate swr context");
        return -1;
    }
    
    if (!v->swr) {
        set_error(v, "swr context is NULL after allocation");
        return -1;
    }
    
    ret = swr_init(v->swr);
    if (ret < 0) {
        set_ff_error(v, ret, "failed to init swr context");
        return -1;
    }
    
    v->out_sample_fmt = AV_SAMPLE_FMT_S16;
    av_opt_get_sample_fmt(v->swr, "out_sample_fmt", 0, &v->out_sample_fmt);

    return 0;
}

VNEVideo *vne_video_open(const char *path, VNEVideoInfo *out_info) {
    if (!path) return NULL;

    VNEVideo *v = (VNEVideo *)calloc(1, sizeof(VNEVideo));
    if (!v) return NULL;

    v->vstream_index = -1;
    v->astream_index = -1;

    av_log_set_level(AV_LOG_ERROR);

    FILE *fp = fopen(path, "rb");
    int64_t data_size = 0;
    int probe = 0;

    if (fp) {
        probe = vne_video_probe_header(fp, &data_size);
        if (probe < 0) {
            set_error(v, "invalid .video header");
            fclose(fp);
            vne_video_close(v);
            return NULL;
        }
    }

    int ret = 0;
    if (probe == 1) {
        VNEVideoIO *io = (VNEVideoIO *)calloc(1, sizeof(VNEVideoIO));
        if (!io) {
            set_error(v, "out of memory for io");
            fclose(fp);
            vne_video_close(v);
            return NULL;
        }
        io->fp = fp;
        io->data_offset = 16;
        io->data_size = data_size;
        io->pos = 0;

        v->io = io;

        const int avio_buf_size = 64 * 1024;
        unsigned char *avio_buf = (unsigned char *)av_malloc((size_t)avio_buf_size);
        if (!avio_buf) {
            set_error(v, "out of memory for avio buffer");
            vne_video_close(v);
            return NULL;
        }

        v->avio = avio_alloc_context(avio_buf, avio_buf_size, 0, io, vne_video_read, NULL, vne_video_seek);
        if (!v->avio) {
            av_free(avio_buf);
            set_error(v, "failed to create avio context");
            vne_video_close(v);
            return NULL;
        }

        v->avio->seekable = AVIO_SEEKABLE_NORMAL;

        v->fmt = avformat_alloc_context();
        if (!v->fmt) {
            set_error(v, "failed to alloc format context");
            vne_video_close(v);
            return NULL;
        }
        v->fmt->pb = v->avio;
        v->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        ret = avformat_open_input(&v->fmt, NULL, NULL, NULL);
        if (ret < 0) {
            set_ff_error(v, ret, "avformat_open_input (custom io) failed");
            vne_video_close(v);
            return NULL;
        }
    } else {
        if (fp) fclose(fp);
        ret = avformat_open_input(&v->fmt, path, NULL, NULL);
        if (ret < 0) {
            set_ff_error(v, ret, "avformat_open_input failed");
            vne_video_close(v);
            return NULL;
        }
    }

    ret = avformat_find_stream_info(v->fmt, NULL);
    if (ret < 0) {
        set_ff_error(v, ret, "avformat_find_stream_info failed");
        vne_video_close(v);
        return NULL;
    }

    if (init_video_decoder(v) < 0) {
        vne_video_close(v);
        return NULL;
    }

    if (init_audio_decoder(v) < 0) {
        vne_video_close(v);
        return NULL;
    }

    v->vframe = av_frame_alloc();
    v->aframe = av_frame_alloc();
    v->pkt = av_packet_alloc();
    if (!v->vframe || !v->aframe || !v->pkt) {
        set_error(v, "failed to allocate frame or packet");
        vne_video_close(v);
        return NULL;
    }

    if (out_info) {
        memset(out_info, 0, sizeof(*out_info));
        out_info->width = v->vdec->width;
        out_info->height = v->vdec->height;

        AVRational fr = av_guess_frame_rate(v->fmt, v->vstream, NULL);
        out_info->fps_num = fr.num;
        out_info->fps_den = fr.den;

        if (v->fmt->duration > 0) {
            out_info->duration_ms = v->fmt->duration / 1000;
        }

        if (v->adec) {
            out_info->has_audio = 1;
            out_info->sample_rate = v->adec->sample_rate;
            out_info->channels = v->adec->ch_layout.nb_channels > 0 ? v->adec->ch_layout.nb_channels : v->adec->channels;
        }
    }

    return v;
}

void vne_video_close(VNEVideo *v) {
    if (!v) return;

    if (v->pkt) av_packet_free(&v->pkt);
    if (v->vframe) av_frame_free(&v->vframe);
    if (v->aframe) av_frame_free(&v->aframe);

    if (v->sws) sws_freeContext(v->sws);
    if (v->swr) swr_free(&v->swr);

    if (v->vdec) avcodec_free_context(&v->vdec);
    if (v->adec) avcodec_free_context(&v->adec);

    if (v->fmt) avformat_close_input(&v->fmt);
    if (v->avio) avio_context_free(&v->avio);
    if (v->io) {
        if (v->io->fp) fclose(v->io->fp);
        free(v->io);
    }

    free(v);
}

const char *vne_video_last_error(VNEVideo *v) {
    if (!v) return "no handle";
    return v->last_error[0] ? v->last_error : "";
}

static int try_receive_video(VNEVideo *v, VNEVideoFrame *out_video) {
    if (!v->vdec || !out_video) return 0;

    VNEF_LOG("[VIDEO] Entering try_receive_video\n");
    fflush(stderr);

    int ret = avcodec_receive_frame(v->vdec, v->vframe);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        VNEF_LOG("[VIDEO] No frame available (EAGAIN or EOF)\n");
        fflush(stderr);
        return 0;
    }
    if (ret < 0) {
        set_ff_error(v, ret, "video receive_frame failed");
        return -1;
    }

    VNEF_LOG("[VIDEO] Got video frame\n");
    fflush(stderr);

    int width = v->vframe->width;
    int height = v->vframe->height;
    enum AVPixelFormat fmt = (enum AVPixelFormat)v->vframe->format;

    if (width <= 0 || height <= 0) {
        set_error(v, "invalid video frame size");
        return -1;
    }

    if (!v->sws || v->sws_w != width || v->sws_h != height || v->sws_fmt != fmt) {
        if (v->sws) sws_freeContext(v->sws);
        v->sws = sws_getContext(
            width,
            height,
            fmt,
            width,
            height,
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,
            NULL,
            NULL,
            NULL
        );
        if (!v->sws) {
            set_error(v, "failed to create sws context");
            return -1;
        }
        v->sws_w = width;
        v->sws_h = height;
        v->sws_fmt = fmt;
    }
    
    uint8_t *dst_data[4] = { 0 };
    int dst_linesize[4] = { 0 };

    // Use av_image_alloc to allocate buffer with proper size and alignment
    int buf_size = av_image_alloc(dst_data, dst_linesize, width, height, AV_PIX_FMT_RGBA, 32);
    if (buf_size < 0) {
        set_ff_error(v, buf_size, "failed to allocate video image buffer");
        return -1;
    }
    
    VNEF_LOG("[VIDEO] av_image_alloc returned size=%d, buffer at %p\n", buf_size, (void*)dst_data[0]);
    fflush(stderr);

    int scaled = sws_scale(v->sws,
        (const uint8_t * const *)v->vframe->data,
        v->vframe->linesize,
        0,
        height,
        dst_data,
        dst_linesize
    );
    if (scaled <= 0) {
        av_freep(&dst_data[0]);
        set_error(v, "sws_scale failed");
        return -1;
    }

    int64_t best_pts = v->vframe->best_effort_timestamp;

    out_video->width = width;
    out_video->height = height;
    out_video->stride = dst_linesize[0];
    out_video->data = dst_data[0];
    out_video->pts_ms = pts_to_ms(v->vstream, best_pts);

    VNEF_LOG("[VIDEO] Returning video buffer %p to caller\n", (void*)dst_data[0]);
    fflush(stderr);

    av_frame_unref(v->vframe);
    return 1;
}

static int try_receive_audio(VNEVideo *v, VNEAudioFrame *out_audio) {
    if (!v->adec || !out_audio) return 0;

    int ret = avcodec_receive_frame(v->adec, v->aframe);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return 0;
    }
    if (ret < 0) {
        set_ff_error(v, ret, "audio receive_frame failed");
        return -1;
    }

    int channels = v->adec->ch_layout.nb_channels > 0 ? v->adec->ch_layout.nb_channels : v->adec->channels;
    int samples = v->aframe->nb_samples;
    
    VNEF_LOG("[AUDIO] Received frame: samples=%d channels=%d fmt=%s\n",
            samples, channels, av_get_sample_fmt_name(v->adec->sample_fmt));
    fflush(stderr);
    
    if (samples <= 0 || channels <= 0) {
        set_error(v, "invalid audio frame: samples or channels <= 0");
        av_frame_unref(v->aframe);
        return -1;
    }

    // Calculate output buffer size
    int64_t delay = swr_get_delay(v->swr, v->adec->sample_rate);
    int max_out_samples = (int)av_rescale_rnd(delay + samples, v->adec->sample_rate, v->adec->sample_rate, AV_ROUND_UP);
    if (max_out_samples <= 0) {
        max_out_samples = samples;
    }
    
    VNEF_LOG("[AUDIO] Calculating buffer: delay=%ld max_out_samples=%d\n", (long)delay, max_out_samples);

    // Allocate output buffer - S16 is interleaved so we only need one buffer
    int buf_size = av_samples_get_buffer_size(NULL, channels, max_out_samples, AV_SAMPLE_FMT_S16, 1);
    if (buf_size < 0) {
        set_ff_error(v, buf_size, "failed to calculate audio buffer size");
        av_frame_unref(v->aframe);
        return -1;
    }
    
    VNEF_LOG("[AUDIO] Allocating %d bytes\n", buf_size);

    uint8_t *out_buf = (uint8_t *)av_malloc((size_t)buf_size);
    if (!out_buf) {
        set_error(v, "failed to allocate audio output buffer");
        av_frame_unref(v->aframe);
        return -1;
    }
    
    VNEF_LOG("[AUDIO] Allocated buffer at %p\n", (void*)out_buf);

    // Set up output pointer array for swr_convert (even for interleaved, needs array)
    uint8_t *out_ptrs[1] = { out_buf };

    // Convert/resample
    int converted = swr_convert(
        v->swr,
        out_ptrs,
        max_out_samples,
        (const uint8_t **)v->aframe->data,
        samples
    );

    if (converted < 0) {
        VNEF_LOG("[AUDIO] swr_convert failed, freeing %p\n", (void*)out_buf);
        set_ff_error(v, converted, "swr_convert failed");
        av_freep(&out_buf);
        av_frame_unref(v->aframe);
        return -1;
    }

    if (converted == 0) {
        VNEF_LOG("[AUDIO] swr_convert returned 0, freeing %p\n", (void*)out_buf);
        set_error(v, "swr_convert returned 0 samples");
        av_freep(&out_buf);
        av_frame_unref(v->aframe);
        return -1;
    }
    
    VNEF_LOG("[AUDIO] Converted %d samples\n", converted);

    // Calculate actual size of converted data
    int actual_size = av_samples_get_buffer_size(NULL, channels, converted, AV_SAMPLE_FMT_S16, 1);
    if (actual_size < 0) {
        VNEF_LOG("[AUDIO] Failed to calc actual size, freeing %p\n", (void*)out_buf);
        set_ff_error(v, actual_size, "failed to calculate converted buffer size");
        av_freep(&out_buf);
        av_frame_unref(v->aframe);
        return -1;
    }
    
    VNEF_LOG("[AUDIO] Actual size: %d bytes (allocated: %d)\n", actual_size, buf_size);

    // DON'T trim - just return the full buffer to avoid any realloc issues

    int64_t best_pts = v->aframe->best_effort_timestamp;

    out_audio->sample_rate = v->adec->sample_rate;
    out_audio->channels = channels;
    out_audio->nb_samples = converted;
    out_audio->bytes_per_sample = 2;
    out_audio->data = out_buf;
    out_audio->pts_ms = pts_to_ms(v->astream, best_pts);
    
    VNEF_LOG("[AUDIO] Returning buffer %p to caller\n", (void*)out_buf);
    fflush(stderr);

    av_frame_unref(v->aframe);
    return 1;
}

VNEFrameType vne_video_next(VNEVideo *v, VNEVideoFrame *out_video, VNEAudioFrame *out_audio) {
    if (!v) return VNE_FRAME_ERROR;

    VNEF_LOG("[NEXT] vne_video_next called, out_video=%p out_audio=%p\n", (void*)out_video, (void*)out_audio);
    fflush(stderr);

    for (;;) {
        VNEF_LOG("[NEXT] Loop iteration: trying video\n");
        fflush(stderr);
        int got = try_receive_video(v, out_video);
        if (got == 1) {
            VNEF_LOG("[NEXT] Returning VIDEO frame\n");
            fflush(stderr);
            return VNE_FRAME_VIDEO;
        }
        if (got < 0) return VNE_FRAME_ERROR;

        VNEF_LOG("[NEXT] Trying audio\n");
        fflush(stderr);
        got = try_receive_audio(v, out_audio);
        if (got == 1) {
            VNEF_LOG("[NEXT] Returning AUDIO frame\n");
            fflush(stderr);
            return VNE_FRAME_AUDIO;
        }
        if (got < 0) return VNE_FRAME_ERROR;

        if (v->eof) {
            return VNE_FRAME_EOF;
        }

        int ret = av_read_frame(v->fmt, v->pkt);
        if (ret == AVERROR_EOF) {
            v->eof = 1;
            if (v->vdec) avcodec_send_packet(v->vdec, NULL);
            if (v->adec) avcodec_send_packet(v->adec, NULL);
            continue;
        }
        if (ret < 0) {
            set_ff_error(v, ret, "av_read_frame failed");
            return VNE_FRAME_ERROR;
        }

        if (v->pkt->stream_index == v->vstream_index) {
            avcodec_send_packet(v->vdec, v->pkt);
        } else if (v->pkt->stream_index == v->astream_index) {
            if (v->adec) avcodec_send_packet(v->adec, v->pkt);
        }

        av_packet_unref(v->pkt);
    }
}

void vne_video_free_video_frame(VNEVideoFrame *f) {
    if (!f) return;
    VNEF_LOG("[VIDEO] Freeing video frame buffer %p\n", (void*)f->data);
    fflush(stderr);
    if (f->data) av_free(f->data);
    f->data = NULL;
    f->width = 0;
    f->height = 0;
    f->stride = 0;
    f->pts_ms = 0;
}

void vne_video_free_audio_frame(VNEAudioFrame *f) {
    if (!f) return;
    if (f->data) {
        VNEF_LOG("[AUDIO] Freeing audio frame buffer %p\n", (void*)f->data);
        av_free(f->data);
    }
    f->data = NULL;
    f->sample_rate = 0;
    f->channels = 0;
    f->nb_samples = 0;
    f->bytes_per_sample = 0;
    f->pts_ms = 0;
}

int vne_video_seek_ms(VNEVideo *v, int64_t target_ms) {
    if (!v || !v->vstream) return -1;

    int64_t ts = av_rescale_q(target_ms, (AVRational){1, 1000}, v->vstream->time_base);
    int ret = av_seek_frame(v->fmt, v->vstream_index, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        set_ff_error(v, ret, "av_seek_frame failed");
        return -1;
    }

    if (v->vdec) avcodec_flush_buffers(v->vdec);
    if (v->adec) avcodec_flush_buffers(v->adec);
    v->eof = 0;
    return 0;
}
