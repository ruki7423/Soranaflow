#include "AudioFingerprinter.h"

#include <QDebug>
#include <QThread>
#include <QCoreApplication>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <chromaprint.h>
}

AudioFingerprinter* AudioFingerprinter::instance()
{
    static AudioFingerprinter s_instance;
    return &s_instance;
}

AudioFingerprinter::AudioFingerprinter(QObject* parent)
    : QObject(parent)
{
    qDebug() << "[AudioFingerprinter] Initialized";
}

void AudioFingerprinter::generateFingerprint(const QString& filePath)
{
    // Run in a worker thread to avoid blocking the UI
    QThread* thread = QThread::create([this, filePath]() {
        AVFormatContext* fmtCtx = nullptr;
        AVCodecContext* codecCtx = nullptr;
        SwrContext* swrCtx = nullptr;
        ChromaprintContext* chromaCtx = nullptr;
        AVFrame* frame = nullptr;
        AVPacket* pkt = nullptr;

        auto cleanup = [&]() {
            if (pkt)      { av_packet_free(&pkt); }
            if (frame)    { av_frame_free(&frame); }
            if (chromaCtx) { chromaprint_free(chromaCtx); }
            if (swrCtx)   { swr_free(&swrCtx); }
            if (codecCtx) { avcodec_free_context(&codecCtx); }
            if (fmtCtx)   { avformat_close_input(&fmtCtx); }
        };

        // Open file
        if (avformat_open_input(&fmtCtx, filePath.toUtf8().constData(),
                                nullptr, nullptr) < 0) {
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("Failed to open file"));
            }, Qt::QueuedConnection);
            return;
        }

        if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
            cleanup();
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("Failed to find stream info"));
            }, Qt::QueuedConnection);
            return;
        }

        // Find audio stream
        int audioStreamIdx = -1;
        const AVCodec* codec = nullptr;
        for (unsigned i = 0; i < fmtCtx->nb_streams; ++i) {
            if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioStreamIdx = static_cast<int>(i);
                codec = avcodec_find_decoder(fmtCtx->streams[i]->codecpar->codec_id);
                break;
            }
        }

        if (audioStreamIdx < 0 || !codec) {
            cleanup();
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("No audio stream found"));
            }, Qt::QueuedConnection);
            return;
        }

        // Open codec
        codecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codecCtx, fmtCtx->streams[audioStreamIdx]->codecpar);
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
            cleanup();
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("Failed to open codec"));
            }, Qt::QueuedConnection);
            return;
        }

        // Set up resampler to 16-bit signed mono 11025 Hz (Chromaprint default)
        const int targetRate = 11025;
        const AVChannelLayout targetChLayout = AV_CHANNEL_LAYOUT_MONO;
        const enum AVSampleFormat targetFmt = AV_SAMPLE_FMT_S16;

        if (swr_alloc_set_opts2(&swrCtx,
                                &targetChLayout, targetFmt, targetRate,
                                &codecCtx->ch_layout, codecCtx->sample_fmt,
                                codecCtx->sample_rate, 0, nullptr) < 0 || !swrCtx) {
            cleanup();
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("Failed to create resampler"));
            }, Qt::QueuedConnection);
            return;
        }
        if (swr_init(swrCtx) < 0) {
            cleanup();
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("Failed to init resampler"));
            }, Qt::QueuedConnection);
            return;
        }

        // Init Chromaprint
        chromaCtx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
        chromaprint_start(chromaCtx, targetRate, 1);

        frame = av_frame_alloc();
        pkt = av_packet_alloc();

        // We only need ~120 seconds for a fingerprint
        const int maxSamples = targetRate * 120;
        int totalSamples = 0;

        while (av_read_frame(fmtCtx, pkt) >= 0 && totalSamples < maxSamples) {
            if (pkt->stream_index != audioStreamIdx) {
                av_packet_unref(pkt);
                continue;
            }

            if (avcodec_send_packet(codecCtx, pkt) < 0) {
                av_packet_unref(pkt);
                continue;
            }

            while (avcodec_receive_frame(codecCtx, frame) == 0) {
                // Resample
                int outSamples = swr_get_out_samples(swrCtx, frame->nb_samples);
                uint8_t* outBuf = nullptr;
                av_samples_alloc(&outBuf, nullptr, 1, outSamples, targetFmt, 0);

                int converted = swr_convert(swrCtx, &outBuf, outSamples,
                                            (const uint8_t**)frame->extended_data,
                                            frame->nb_samples);
                if (converted > 0) {
                    chromaprint_feed(chromaCtx, reinterpret_cast<const int16_t*>(outBuf),
                                    converted);
                    totalSamples += converted;
                }

                av_freep(&outBuf);
                av_frame_unref(frame);

                if (totalSamples >= maxSamples)
                    break;
            }
            av_packet_unref(pkt);
        }

        chromaprint_finish(chromaCtx);

        // Get fingerprint
        char* rawFp = nullptr;
        if (chromaprint_get_fingerprint(chromaCtx, &rawFp) != 1 || !rawFp) {
            cleanup();
            QMetaObject::invokeMethod(this, [this, filePath]() {
                emit fingerprintError(filePath, QStringLiteral("Failed to get fingerprint"));
            }, Qt::QueuedConnection);
            return;
        }

        QString fingerprint = QString::fromUtf8(rawFp);
        chromaprint_dealloc(rawFp);

        qDebug() << "[AudioFingerprinter] Generated fingerprint for:" << filePath
                 << "length:" << fingerprint.length()
                 << "totalSamples:" << totalSamples;

        int durationSecs = 0;
        if (fmtCtx->duration != AV_NOPTS_VALUE) {
            durationSecs = static_cast<int>(fmtCtx->duration / AV_TIME_BASE);
        } else {
            durationSecs = totalSamples / targetRate;
        }

        cleanup();

        QMetaObject::invokeMethod(this, [this, filePath, fingerprint, durationSecs]() {
            emit fingerprintReady(filePath, fingerprint, durationSecs);
        }, Qt::QueuedConnection);
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}
