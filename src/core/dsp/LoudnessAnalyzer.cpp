#include "LoudnessAnalyzer.h"

#include <ebur128.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
}

#include <QDebug>
#include <cmath>
#include <vector>

LoudnessResult LoudnessAnalyzer::analyze(const QString& filePath)
{
    LoudnessResult result;

    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, filePath.toUtf8().constData(), nullptr, nullptr) < 0)
        return result;

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return result;
    }

    int audioIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIdx < 0) {
        avformat_close_input(&fmt);
        return result;
    }

    AVCodecParameters* par = fmt->streams[audioIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return result;
    }

    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, par);
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return result;
    }

    int sr = ctx->sample_rate;
    int ch = ctx->ch_layout.nb_channels;

    // Init ebur128
    ebur128_state* state = ebur128_init(
        static_cast<unsigned int>(ch),
        static_cast<unsigned long>(sr),
        EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
    if (!state) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return result;
    }

    // Setup resampler to float interleaved
    SwrContext* swr = nullptr;
    AVChannelLayout outLayout{};
    av_channel_layout_default(&outLayout, ch);
    swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, sr,
                        &ctx->ch_layout, ctx->sample_fmt, sr, 0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        ebur128_destroy(&state);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return result;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // Pre-allocate a decode buffer to avoid per-frame allocation
    std::vector<float> outBuf;

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == audioIdx) {
            avcodec_send_packet(ctx, pkt);
            while (avcodec_receive_frame(ctx, frame) >= 0) {
                int outSamples = frame->nb_samples;
                int needed = outSamples * ch;
                if ((int)outBuf.size() < needed)
                    outBuf.resize(needed);
                uint8_t* outPtr = reinterpret_cast<uint8_t*>(outBuf.data());
                swr_convert(swr, &outPtr, outSamples,
                            const_cast<const uint8_t**>(frame->extended_data), outSamples);
                ebur128_add_frames_float(state, outBuf.data(), outSamples);
            }
        }
        av_packet_unref(pkt);
    }

    // Get results
    double loudness = 0.0;
    if (ebur128_loudness_global(state, &loudness) == EBUR128_SUCCESS) {
        result.integratedLoudness = loudness;
        double peak = 0.0;
        for (int i = 0; i < ch; i++) {
            double chPeak = 0.0;
            ebur128_true_peak(state, static_cast<unsigned int>(i), &chPeak);
            if (chPeak > peak) peak = chPeak;
        }
        result.truePeak = (peak > 0) ? 20.0 * std::log10(peak) : -100.0;
        result.valid = true;
        qDebug() << "[R128] Analyzed:" << filePath
                 << "loudness:" << loudness << "LUFS"
                 << "peak:" << result.truePeak << "dBTP";
    }

    ebur128_destroy(&state);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    av_channel_layout_uninit(&outLayout);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);

    return result;
}
