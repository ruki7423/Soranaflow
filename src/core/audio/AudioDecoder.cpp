#include "AudioDecoder.h"
#include <QString>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>
#include <memory>

struct AudioDecoder::Impl {
    AVFormatContext* fmtCtx   = nullptr;
    AVCodecContext*  codecCtx = nullptr;
    SwrContext*      swrCtx   = nullptr;
    AVPacket*        packet   = nullptr;
    AVFrame*         frame    = nullptr;

    int              audioStreamIndex = -1;
    AudioStreamFormat streamFormat;
    int64_t          framesDecoded = 0;  // total frames output so far
    bool             opened = false;

    // Residual buffer for partial reads (RAII)
    std::unique_ptr<float[]> residualBuf;
    int              residualFrames = 0;
    int              residualOffset = 0;

    ~Impl() {
        cleanup();
    }

    void cleanup() {
        residualBuf.reset();
        if (frame)    { av_frame_free(&frame); }
        if (packet)   { av_packet_free(&packet); }
        if (swrCtx)   { swr_free(&swrCtx); }
        if (codecCtx) { avcodec_free_context(&codecCtx); }
        if (fmtCtx)   { avformat_close_input(&fmtCtx); }
        audioStreamIndex = -1;
        framesDecoded = 0;
        residualFrames = 0;
        residualOffset = 0;
        opened = false;
    }
};

AudioDecoder::AudioDecoder()
    : m_impl(std::make_unique<Impl>())
{
}

AudioDecoder::~AudioDecoder() = default;

bool AudioDecoder::open(const std::string& filePath)
{
    close();

    auto& d = *m_impl;

    // Open input
    if (avformat_open_input(&d.fmtCtx, filePath.c_str(), nullptr, nullptr) < 0)
        return false;

    if (avformat_find_stream_info(d.fmtCtx, nullptr) < 0) {
        avformat_close_input(&d.fmtCtx);
        return false;
    }

    // Find best audio stream
    d.audioStreamIndex = av_find_best_stream(d.fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (d.audioStreamIndex < 0) {
        avformat_close_input(&d.fmtCtx);
        return false;
    }

    AVStream* stream = d.fmtCtx->streams[d.audioStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&d.fmtCtx);
        return false;
    }

    d.codecCtx = avcodec_alloc_context3(codec);
    if (!d.codecCtx) {
        avformat_close_input(&d.fmtCtx);
        return false;
    }
    if (avcodec_parameters_to_context(d.codecCtx, stream->codecpar) < 0) {
        avcodec_free_context(&d.codecCtx);
        avformat_close_input(&d.fmtCtx);
        return false;
    }

    if (avcodec_open2(d.codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&d.codecCtx);
        avformat_close_input(&d.fmtCtx);
        return false;
    }

    // Setup resampler: convert to interleaved float32 stereo
    int outChannels = d.codecCtx->ch_layout.nb_channels;
    if (outChannels == 0) outChannels = 2;

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels);

    // Detect DSD codecs and set appropriate output sample rate
    int outSampleRate = d.codecCtx->sample_rate;
    bool isDSD = false;
    switch (d.codecCtx->codec_id) {
    case AV_CODEC_ID_DSD_LSBF:
    case AV_CODEC_ID_DSD_MSBF:
    case AV_CODEC_ID_DSD_LSBF_PLANAR:
    case AV_CODEC_ID_DSD_MSBF_PLANAR:
        isDSD = true;
        // DSD sample rates are in the MHz range; decimate to PCM
        // DSD64 (2.8MHz) -> 176400, DSD128 (5.6MHz) -> 176400, etc.
        outSampleRate = 176400;
        break;
    default:
        break;
    }

    int ret = swr_alloc_set_opts2(
        &d.swrCtx,
        &outLayout,                    // out layout
        AV_SAMPLE_FMT_FLT,            // out format: interleaved float32
        outSampleRate,                 // out sample rate (176.4kHz for DSD)
        &d.codecCtx->ch_layout,        // in layout
        d.codecCtx->sample_fmt,        // in format
        d.codecCtx->sample_rate,       // in sample rate
        0, nullptr
    );

    if (ret < 0 || swr_init(d.swrCtx) < 0) {
        d.cleanup();
        return false;
    }

    d.packet = av_packet_alloc();
    d.frame  = av_frame_alloc();
    if (!d.packet || !d.frame) {
        d.cleanup();
        return false;
    }

    // Fill format info — use output sample rate (which differs for DSD)
    d.streamFormat.sampleRate    = outSampleRate;
    d.streamFormat.channels      = outChannels;
    d.streamFormat.bitsPerSample = isDSD ? 32
                                   : (d.codecCtx->bits_per_raw_sample > 0
                                      ? d.codecCtx->bits_per_raw_sample : 16);

    if (stream->duration != AV_NOPTS_VALUE) {
        double tb = av_q2d(stream->time_base);
        d.streamFormat.durationSecs = stream->duration * tb;
    } else if (d.fmtCtx->duration != AV_NOPTS_VALUE) {
        d.streamFormat.durationSecs = d.fmtCtx->duration / (double)AV_TIME_BASE;
    }

    d.streamFormat.totalFrames = (int64_t)(d.streamFormat.durationSecs * d.streamFormat.sampleRate);

    // Allocate residual buffer (max one decoded frame worth of data)
    // Use a generous size: 192000 samples * channels
    d.residualBuf = std::make_unique<float[]>(192000 * outChannels);

    d.opened = true;
    return true;
}

void AudioDecoder::close()
{
    m_impl->cleanup();
}

bool AudioDecoder::isOpen() const
{
    return m_impl->opened;
}

int AudioDecoder::read(float* buf, int maxFrames)
{
    auto& d = *m_impl;
    if (!d.opened) return 0;

    int channels = d.streamFormat.channels;
    int framesWritten = 0;

    // First, drain residual buffer
    if (d.residualFrames > 0) {
        int toCopy = std::min(d.residualFrames, maxFrames);
        std::memcpy(buf, d.residualBuf.get() + d.residualOffset * channels,
                    toCopy * channels * sizeof(float));
        d.residualOffset += toCopy;
        d.residualFrames -= toCopy;
        framesWritten += toCopy;
        if (framesWritten >= maxFrames) {
            d.framesDecoded += framesWritten;
            return framesWritten;
        }
    }

    // Decode more data
    while (framesWritten < maxFrames) {
        int ret = av_read_frame(d.fmtCtx, d.packet);
        if (ret == AVERROR_EOF) break;  // normal end of file
        if (ret < 0) { av_packet_unref(d.packet); break; }  // decode error

        if (d.packet->stream_index != d.audioStreamIndex) {
            av_packet_unref(d.packet);
            continue;
        }

        ret = avcodec_send_packet(d.codecCtx, d.packet);
        av_packet_unref(d.packet);
        if (ret < 0) continue;

        while (avcodec_receive_frame(d.codecCtx, d.frame) == 0) {
            // Convert frame to float32 interleaved
            int outSamples = d.frame->nb_samples;
            float* outBuf = d.residualBuf.get();

            int converted = swr_convert(d.swrCtx,
                                        (uint8_t**)&outBuf, outSamples,
                                        (const uint8_t**)d.frame->extended_data,
                                        d.frame->nb_samples);
            av_frame_unref(d.frame);
            if (converted <= 0) continue;

            int remaining = maxFrames - framesWritten;
            int toCopy = std::min(converted, remaining);
            std::memcpy(buf + framesWritten * channels,
                        d.residualBuf.get(),
                        toCopy * channels * sizeof(float));
            framesWritten += toCopy;

            // Store leftover in residual
            if (converted > toCopy) {
                d.residualOffset = toCopy;
                d.residualFrames = converted - toCopy;
                // Move residual data to beginning of buffer
                std::memmove(d.residualBuf.get(),
                             d.residualBuf.get() + toCopy * channels,
                             d.residualFrames * channels * sizeof(float));
                d.residualOffset = 0;
            }

            if (framesWritten >= maxFrames) break;
        }
    }

    d.framesDecoded += framesWritten;
    return framesWritten;
}

bool AudioDecoder::seek(double secs)
{
    auto& d = *m_impl;
    if (!d.opened) return false;

    AVStream* stream = d.fmtCtx->streams[d.audioStreamIndex];
    int64_t ts = (int64_t)(secs / av_q2d(stream->time_base));

    if (av_seek_frame(d.fmtCtx, d.audioStreamIndex, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return false;

    avcodec_flush_buffers(d.codecCtx);
    d.residualFrames = 0;
    d.residualOffset = 0;
    d.framesDecoded = (int64_t)(secs * d.streamFormat.sampleRate);
    return true;
}

AudioStreamFormat AudioDecoder::format() const
{
    return m_impl->streamFormat;
}

double AudioDecoder::currentPositionSecs() const
{
    auto& d = *m_impl;
    if (d.streamFormat.sampleRate <= 0) return 0.0;
    return (double)d.framesDecoded / d.streamFormat.sampleRate;
}

QString AudioDecoder::codecName() const
{
    auto& d = *m_impl;
    if (!d.opened || !d.codecCtx) return QString();
    return QString::fromUtf8(avcodec_get_name(d.codecCtx->codec_id));
}
