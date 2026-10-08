#include "gs_aac.h"

#include <SDL3/SDL.h>
#include <libavcodec/avcodec.h>
#include <string.h>

struct gs_aac {
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
    SDL_AudioStream *resample;  // from the stream's rate to the output rate
    int in_rate, out_rate;
    float *stereo;
    int stereo_cap;
};

gs_aac *gs_aac_new(const gs_aac_config *c, int out_rate) {
    const AVCodec *aac = avcodec_find_decoder(AV_CODEC_ID_AAC);
    gs_aac *a = SDL_calloc(1, sizeof *a);
    if (!a || !aac) return SDL_free(a), NULL;
    a->codec = avcodec_alloc_context3(aac);
    a->packet = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (!a->codec || !a->packet || !a->frame) return gs_aac_free(a), NULL;
    a->codec->extradata = av_mallocz((size_t)c->asc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    if (a->codec->extradata) memcpy(a->codec->extradata, c->asc, (size_t)c->asc_len), a->codec->extradata_size = c->asc_len;
    a->codec->sample_rate = c->rate;
    a->in_rate = c->rate, a->out_rate = out_rate;
    if (avcodec_open2(a->codec, aac, NULL) < 0) return gs_aac_free(a), NULL;
    if (c->rate != out_rate) {
        SDL_AudioSpec in = { SDL_AUDIO_F32, 2, c->rate }, out = { SDL_AUDIO_F32, 2, out_rate };
        a->resample = SDL_CreateAudioStream(&in, &out);
        if (!a->resample) return gs_aac_free(a), NULL;
    }
    return a;
}

void gs_aac_free(gs_aac *a) {
    if (!a) return;
    avcodec_free_context(&a->codec);
    av_packet_free(&a->packet);
    av_frame_free(&a->frame);
    SDL_DestroyAudioStream(a->resample);
    SDL_free(a->stereo);
    SDL_free(a);
}

void gs_aac_flush(gs_aac *a) {
    avcodec_flush_buffers(a->codec);
    if (a->resample) SDL_ClearAudioStream(a->resample);
}

// The decoded frame as interleaved stereo: planar or packed floats, mono doubled, extra channels dropped.
static int to_stereo(gs_aac *a, const AVFrame *f) {
    int n = f->nb_samples, channels = f->ch_layout.nb_channels;
    if (n > a->stereo_cap) {
        float *grown = SDL_realloc(a->stereo, sizeof *grown * 2 * (size_t)n);
        if (!grown) return 0;
        a->stereo = grown, a->stereo_cap = n;
    }
    bool planar = f->format == AV_SAMPLE_FMT_FLTP;
    if (f->format != AV_SAMPLE_FMT_FLTP && f->format != AV_SAMPLE_FMT_FLT) return 0;
    for (int i = 0; i < n; i++) {
        float l = planar ? ((const float *)f->data[0])[i] : ((const float *)f->data[0])[i * channels];
        float r = channels < 2 ? l : planar ? ((const float *)f->data[1])[i] : ((const float *)f->data[0])[i * channels + 1];
        a->stereo[2 * i] = l, a->stereo[2 * i + 1] = r;
    }
    return n;
}

int gs_aac_decode(gs_aac *a, const uint8_t *frame, size_t len, float *lr, int max_frames) {
    if (av_new_packet(a->packet, (int)len) < 0) return -1;
    memcpy(a->packet->data, frame, len);
    int sent = avcodec_send_packet(a->codec, a->packet);
    av_packet_unref(a->packet);
    if (sent < 0 && sent != AVERROR_INVALIDDATA && sent != AVERROR(EAGAIN)) return -1;
    int written = 0;
    while (avcodec_receive_frame(a->codec, a->frame) == 0) {
        int n = to_stereo(a, a->frame);
        if (a->resample) {
            SDL_PutAudioStreamData(a->resample, a->stereo, n * 2 * (int)sizeof(float));
        } else {
            int k = n < max_frames - written ? n : max_frames - written;
            memcpy(lr + 2 * written, a->stereo, sizeof(float) * 2 * (size_t)k);
            written += k;
        }
        av_frame_unref(a->frame);
    }
    if (a->resample) {
        int got = SDL_GetAudioStreamData(a->resample, lr, max_frames * 2 * (int)sizeof(float));
        written = got > 0 ? got / (2 * (int)sizeof(float)) : 0;
    }
    return written;
}
