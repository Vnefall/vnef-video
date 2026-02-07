package main

import "core:fmt"
import "core:os"
import "core:c"
import vnef_video "../bindings/odin"

main :: proc() {
    if len(os.args) < 2 {
        fmt.println("Usage: odin run examples/odin_dump.odin -- <video-file>")
        return
    }

    path := os.args[1]
    info: vnef_video.VNEVideoInfo

    v := vnef_video.vne_video_open(cstring(path), &info)
    if v == nil {
        fmt.println("Failed to open video")
        return
    }

    fmt.printf("Video: %dx%d fps=%d/%d duration=%dms\n", info.width, info.height, info.fps_num, info.fps_den, info.duration_ms)
    if info.has_audio != 0 {
        fmt.printf("Audio: %d Hz, %d channels\n", info.sample_rate, info.channels)
    } else {
        fmt.println("Audio: none")
    }

    vcount := 0
    acount := 0

    for vcount < 3 || acount < 3 {
        vf := vnef_video.VNEVideoFrame{}
        af := vnef_video.VNEAudioFrame{}
        t := vnef_video.vne_video_next(v, &vf, &af)

        switch t {
        case .VNE_FRAME_VIDEO:
            fmt.printf("Video frame %d pts=%dms\n", vcount, vf.pts_ms)
            vnef_video.vne_video_free_video_frame(&vf)
            vcount += 1
        case .VNE_FRAME_AUDIO:
            fmt.printf("Audio frame %d pts=%dms samples=%d\n", acount, af.pts_ms, af.nb_samples)
            vnef_video.vne_video_free_audio_frame(&af)
            acount += 1
        case .VNE_FRAME_EOF:
            fmt.println("EOF")
            break
        case .VNE_FRAME_ERROR:
            err := vnef_video.vne_video_last_error(v)
            fmt.printf("Error: %s\n", err)
            break
        case .VNE_FRAME_NONE:
            // no-op
        }
    }

    vnef_video.vne_video_close(v)
}
