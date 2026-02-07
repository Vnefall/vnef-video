package vnef_video

import "core:c"

VNEFrameType :: enum c.int {
    VNE_FRAME_ERROR = -1,
    VNE_FRAME_NONE  = 0,
    VNE_FRAME_VIDEO = 1,
    VNE_FRAME_AUDIO = 2,
    VNE_FRAME_EOF   = 3,
}

VNEVideo :: struct { _ : u8 }

VNEVideoInfo :: struct {
    width:      c.int,
    height:     c.int,
    fps_num:    c.int,
    fps_den:    c.int,
    duration_ms: i64,
    has_audio:  c.int,
    sample_rate: c.int,
    channels:   c.int,
}

VNEVideoFrame :: struct {
    width:  c.int,
    height: c.int,
    stride: c.int,
    pts_ms: i64,
    data:   ^u8, // RGBA
}

VNEAudioFrame :: struct {
    sample_rate:     c.int,
    channels:        c.int,
    nb_samples:      c.int,
    bytes_per_sample:c.int,
    pts_ms:          i64,
    data:            ^u8, // interleaved S16
}

foreign import vnef_video "vnef_video"

foreign vnef_video {
    vne_video_open             :: proc(path: cstring, out_info: ^VNEVideoInfo) -> ^VNEVideo
    vne_video_close            :: proc(v: ^VNEVideo)
    vne_video_last_error       :: proc(v: ^VNEVideo) -> cstring

    vne_video_next             :: proc(v: ^VNEVideo, out_video: ^VNEVideoFrame, out_audio: ^VNEAudioFrame) -> VNEFrameType

    vne_video_free_video_frame :: proc(f: ^VNEVideoFrame)
    vne_video_free_audio_frame :: proc(f: ^VNEAudioFrame)

    vne_video_seek_ms          :: proc(v: ^VNEVideo, target_ms: i64) -> c.int
}
