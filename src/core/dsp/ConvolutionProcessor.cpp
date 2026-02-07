#include "ConvolutionProcessor.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <QDebug>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Constructor / Destructor ────────────────────────────────────────

#ifdef __APPLE__

ConvolutionProcessor::ConvolutionProcessor()
{
    // FFT_LOG2N = 11, so this supports up to 2^11 = 2048 point FFT
    m_fftSetup = vDSP_create_fftsetup(FFT_LOG2N, kFFTRadix2);

    // vDSP_fft_zrip operates on FFT_HALF complex pairs for FFT_SIZE real values
    m_fftInBuf.resize(FFT_SIZE, 0.0f);
    m_fftSplitReal.resize(FFT_HALF, 0.0f);
    m_fftSplitImag.resize(FFT_HALF, 0.0f);
    m_fftSplit.realp = m_fftSplitReal.data();
    m_fftSplit.imagp = m_fftSplitImag.data();

    m_accumReal.resize(FFT_HALF, 0.0f);
    m_accumImag.resize(FFT_HALF, 0.0f);
    m_accumSplit.realp = m_accumReal.data();
    m_accumSplit.imagp = m_accumImag.data();

    m_tempReal.resize(FFT_HALF, 0.0f);
    m_tempImag.resize(FFT_HALF, 0.0f);
    m_tempSplit.realp = m_tempReal.data();
    m_tempSplit.imagp = m_tempImag.data();

    m_ifftOut.resize(FFT_SIZE, 0.0f);

    // Pre-allocate per-channel buffers for MAX_CHANNELS
    m_input.resize(MAX_CHANNELS);
    m_overlap.resize(MAX_CHANNELS);
    m_output.resize(MAX_CHANNELS);
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        m_input[ch].resize(PARTITION_SIZE, 0.0f);
        m_overlap[ch].resize(PARTITION_SIZE, 0.0f);
        m_output[ch].resize(PARTITION_SIZE, 0.0f);
    }

    m_pendingIR = &m_irSlotB;
    m_activeIR = nullptr;
}

ConvolutionProcessor::~ConvolutionProcessor()
{
    if (m_fftSetup) {
        vDSP_destroy_fftsetup(m_fftSetup);
    }
}

#else // !__APPLE__

ConvolutionProcessor::ConvolutionProcessor()
{
    qDebug() << "[ConvolutionProcessor] FFT disabled — vDSP not available, passthrough mode";
}

ConvolutionProcessor::~ConvolutionProcessor() = default;

#endif // __APPLE__

// ── Thread-safe controls ────────────────────────────────────────────

void ConvolutionProcessor::setEnabled(bool enabled)
{
    if (enabled && !m_enabled.load(std::memory_order_relaxed)) {
        m_needsStateReset.store(true, std::memory_order_relaxed);
    }
    m_enabled.store(enabled, std::memory_order_relaxed);
}

void ConvolutionProcessor::setSampleRate(int rate)
{
    m_sampleRate = rate;
    m_needsRecalc.store(true, std::memory_order_relaxed);
}

std::string ConvolutionProcessor::irFilePath() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_irFilePathMutex));
    return m_irFilePath;
}

// ── IR Partition Building ───────────────────────────────────────────

#ifdef __APPLE__

void ConvolutionProcessor::buildIRPartitions(IRData* dest,
                                             const std::vector<std::vector<float>>& irChannels,
                                             int irSampleRate)
{
    int numCh = static_cast<int>(irChannels.size());
    if (numCh < 1) return;
    int irLen = static_cast<int>(irChannels[0].size());
    int numPartitions = (irLen + PARTITION_SIZE - 1) / PARTITION_SIZE;
    if (numPartitions < 1) numPartitions = 1;

    dest->numPartitions = numPartitions;
    dest->channelCount = numCh;
    dest->sampleRate = irSampleRate;

    dest->partitions.resize(numCh);
    dest->reals.resize(numCh);
    dest->imags.resize(numCh);

    // Temporary FFT setup for loading (don't use render-thread m_fftSetup)
    FFTSetup loadSetup = vDSP_create_fftsetup(FFT_LOG2N, kFFTRadix2);
    std::vector<float> padded(FFT_SIZE, 0.0f);

    for (int c = 0; c < numCh; ++c) {
        dest->partitions[c].resize(numPartitions);
        dest->reals[c].resize(numPartitions);
        dest->imags[c].resize(numPartitions);

        for (int p = 0; p < numPartitions; ++p) {
            int offset = p * PARTITION_SIZE;
            int len = std::min(PARTITION_SIZE, irLen - offset);

            dest->reals[c][p].resize(FFT_HALF, 0.0f);
            dest->imags[c][p].resize(FFT_HALF, 0.0f);

            dest->partitions[c][p].realp = dest->reals[c][p].data();
            dest->partitions[c][p].imagp = dest->imags[c][p].data();

            std::memset(padded.data(), 0, FFT_SIZE * sizeof(float));
            if (offset < static_cast<int>(irChannels[c].size())) {
                int copyLen = std::min(len, static_cast<int>(irChannels[c].size()) - offset);
                std::memcpy(padded.data(), irChannels[c].data() + offset, copyLen * sizeof(float));
            }

            DSPSplitComplex split;
            split.realp = dest->reals[c][p].data();
            split.imagp = dest->imags[c][p].data();
            vDSP_ctoz(reinterpret_cast<const DSPComplex*>(padded.data()), 2,
                       &split, 1, FFT_HALF);
            vDSP_fft_zrip(loadSetup, &split, 1, FFT_LOG2N, kFFTDirection_Forward);
        }
    }

    vDSP_destroy_fftsetup(loadSetup);
}

#endif // __APPLE__

// ── IR Loading (FFmpeg) ─────────────────────────────────────────────

bool ConvolutionProcessor::loadIR(const std::string& filePath)
{
#ifdef __APPLE__
    // Wait for any pending swap to be consumed by render thread
    int waitCount = 0;
    while (m_irSwapPending.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (++waitCount > 2000) {  // 2 second timeout
            qDebug() << "[Convolution] Timeout waiting for IR swap";
            return false;
        }
    }
#endif

    // Use FFmpeg to decode any audio format
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.c_str(), nullptr, nullptr) < 0) {
        qDebug() << "[Convolution] Failed to open:" << filePath.c_str();
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    int audioIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioIdx < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecParameters* par = fmtCtx->streams[audioIdx]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, par);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    int irSampleRate = codecCtx->sample_rate;
    int irChannels = codecCtx->ch_layout.nb_channels;
    if (irChannels < 1) irChannels = 1;
    // Clamp to MAX_CHANNELS
    int outChannels = std::min(irChannels, MAX_CHANNELS);

    // Set up resampler to output float interleaved at native channel count
    SwrContext* swr = nullptr;
    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, outChannels);

    swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, irSampleRate,
                        &codecCtx->ch_layout, codecCtx->sample_fmt, irSampleRate, 0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        if (swr) swr_free(&swr);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    // Decode all frames
    std::vector<float> allSamples;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == audioIdx) {
            if (avcodec_send_packet(codecCtx, pkt) >= 0) {
                while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                    int maxOut = frame->nb_samples + 256;
                    std::vector<float> tmp(maxOut * outChannels);
                    uint8_t* outPtr = reinterpret_cast<uint8_t*>(tmp.data());
                    int got = swr_convert(swr, &outPtr, maxOut,
                                const_cast<const uint8_t**>(frame->extended_data),
                                frame->nb_samples);
                    if (got > 0) {
                        allSamples.insert(allSamples.end(),
                                          tmp.begin(), tmp.begin() + got * outChannels);
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }

    // Flush decoder
    avcodec_send_packet(codecCtx, nullptr);
    while (avcodec_receive_frame(codecCtx, frame) >= 0) {
        int maxOut = frame->nb_samples + 256;
        std::vector<float> tmp(maxOut * outChannels);
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(tmp.data());
        int got = swr_convert(swr, &outPtr, maxOut,
                    const_cast<const uint8_t**>(frame->extended_data),
                    frame->nb_samples);
        if (got > 0) {
            allSamples.insert(allSamples.end(),
                              tmp.begin(), tmp.begin() + got * outChannels);
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (allSamples.empty()) {
        qDebug() << "[Convolution] No samples decoded from:" << filePath.c_str();
        return false;
    }

    // Deinterleave into per-channel vectors
    int totalFrames = static_cast<int>(allSamples.size()) / outChannels;
    std::vector<std::vector<float>> irCh(outChannels, std::vector<float>(totalFrames));
    for (int i = 0; i < totalFrames; ++i) {
        for (int c = 0; c < outChannels; ++c) {
            irCh[c][i] = allSamples[i * outChannels + c];
        }
    }

    qDebug() << "[Convolution] IR decoded:" << filePath.c_str()
             << "frames:" << totalFrames
             << "channels:" << outChannels
             << "rate:" << irSampleRate;

#ifdef __APPLE__
    // Determine which slot is NOT active (safe to write)
    IRData* pending;
    {
        std::lock_guard<std::mutex> lock(m_irSwapMutex);
        pending = (m_activeIR == &m_irSlotA) ? &m_irSlotB : &m_irSlotA;
        m_pendingIR = pending;
    }

    buildIRPartitions(pending, irCh, irSampleRate);

    // Signal render thread to swap
    m_irSwapPending.store(true, std::memory_order_release);

    qDebug() << "[Convolution] IR loaded:" << filePath.c_str()
             << "partitions:" << pending->numPartitions
             << "irChannels:" << pending->channelCount;
#else
    qDebug() << "[Convolution] IR file registered (passthrough — vDSP not available)";
#endif

    // Store file path
    {
        std::lock_guard<std::mutex> lock(m_irFilePathMutex);
        m_irFilePath = filePath;
    }

    m_hasIR.store(true, std::memory_order_relaxed);
    m_needsStateReset.store(true, std::memory_order_relaxed);

    return true;
}

void ConvolutionProcessor::clearIR()
{
    m_hasIR.store(false, std::memory_order_relaxed);
    m_enabled.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_irFilePathMutex);
        m_irFilePath.clear();
    }
}

// ── Render thread state management ──────────────────────────────────

#ifdef __APPLE__

void ConvolutionProcessor::resetState()
{
    m_phase = 0;
    m_hasOutput = false;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch) {
        std::memset(m_input[ch].data(), 0, PARTITION_SIZE * sizeof(float));
        std::memset(m_overlap[ch].data(), 0, PARTITION_SIZE * sizeof(float));
        std::memset(m_output[ch].data(), 0, PARTITION_SIZE * sizeof(float));
    }
    m_fdlIdx = 0;
    m_wetMix = 0.0f;

    // Clear FDL buffers
    for (auto& chReals : m_fdlReals)
        for (auto& v : chReals) std::memset(v.data(), 0, v.size() * sizeof(float));
    for (auto& chImags : m_fdlImags)
        for (auto& v : chImags) std::memset(v.data(), 0, v.size() * sizeof(float));
}

// ── Convolution of one channel partition ────────────────────────────

void ConvolutionProcessor::convolveChannel(
    const std::vector<DSPSplitComplex>& irPartitions,
    std::vector<DSPSplitComplex>& fdl,
    int fdlIdx, int numPartitions,
    const float* input, float* output, float* overlap)
{
    if (static_cast<int>(fdl.size()) < numPartitions) return;

    // Zero-pad input to FFT_SIZE: [input | zeros]
    std::memcpy(m_fftInBuf.data(), input, PARTITION_SIZE * sizeof(float));
    std::memset(m_fftInBuf.data() + PARTITION_SIZE, 0, PARTITION_SIZE * sizeof(float));

    // Pack real data into split complex format and forward FFT
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(m_fftInBuf.data()), 2,
               &m_fftSplit, 1, FFT_HALF);
    vDSP_fft_zrip(m_fftSetup, &m_fftSplit, 1, FFT_LOG2N, kFFTDirection_Forward);

    // Store in FDL at current index
    std::memcpy(fdl[fdlIdx].realp, m_fftSplit.realp, FFT_HALF * sizeof(float));
    std::memcpy(fdl[fdlIdx].imagp, m_fftSplit.imagp, FFT_HALF * sizeof(float));

    // Clear accumulator
    std::memset(m_accumReal.data(), 0, FFT_HALF * sizeof(float));
    std::memset(m_accumImag.data(), 0, FFT_HALF * sizeof(float));

    // Accumulate: sum over all partitions of FDL[k] * IR[k]
    for (int p = 0; p < numPartitions; ++p) {
        int fdlSlot = (fdlIdx - p + numPartitions) % numPartitions;

        const float* ar = fdl[fdlSlot].realp;
        const float* ai = fdl[fdlSlot].imagp;
        const float* br = irPartitions[p].realp;
        const float* bi = irPartitions[p].imagp;
        float* cr = m_accumSplit.realp;
        float* ci = m_accumSplit.imagp;

        // Bin 0: DC and Nyquist are independent reals packed together
        cr[0] += ar[0] * br[0];          // DC * DC
        ci[0] += ai[0] * bi[0];          // Nyquist * Nyquist

        // Bins 1..FFT_HALF-1: standard complex multiply-accumulate
        for (int k = 1; k < FFT_HALF; ++k) {
            cr[k] += ar[k] * br[k] - ai[k] * bi[k];
            ci[k] += ar[k] * bi[k] + ai[k] * br[k];
        }
    }

    // Inverse FFT
    vDSP_fft_zrip(m_fftSetup, &m_accumSplit, 1, FFT_LOG2N, kFFTDirection_Inverse);

    // Unpack split complex back to interleaved real
    vDSP_ztoc(&m_accumSplit, 1,
              reinterpret_cast<DSPComplex*>(m_ifftOut.data()), 2, FFT_HALF);

    // Scale: empirically verified by selfTest(): scale = 1.0 / (FFT_SIZE * 4)
    float scale = 1.0f / static_cast<float>(FFT_SIZE * 4);
    vDSP_vsmul(m_ifftOut.data(), 1, &scale, m_ifftOut.data(), 1, FFT_SIZE);

    // Overlap-add: first half + previous overlap → output
    vDSP_vadd(m_ifftOut.data(), 1, overlap, 1, output, 1, PARTITION_SIZE);

    // Save second half as overlap for next block
    std::memcpy(overlap, m_ifftOut.data() + PARTITION_SIZE, PARTITION_SIZE * sizeof(float));
}

// ── Process (render thread) ─────────────────────────────────────────

void ConvolutionProcessor::process(float* buffer, int frameCount, int channels)
{
    bool wantEnabled = m_enabled.load(std::memory_order_relaxed);
    bool hasIR = m_hasIR.load(std::memory_order_relaxed);

    // Skip if disabled and fully faded out
    if ((!wantEnabled || !hasIR) && m_wetMix <= 0.0f) return;

    // Clamp channels
    if (channels < 1 || channels > MAX_CHANNELS) return;
    m_activeChannels = channels;

    // Swap IR if pending — resize FDL BEFORE activating new IR
    if (m_irSwapPending.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(m_irSwapMutex);

        IRData* newIR = m_pendingIR;
        if (newIR && newIR->numPartitions > 0) {
            int n = newIR->numPartitions;
            int numCh = channels;

            m_fdl.resize(numCh);
            m_fdlReals.resize(numCh);
            m_fdlImags.resize(numCh);
            for (int c = 0; c < numCh; ++c) {
                m_fdl[c].resize(n);
                m_fdlReals[c].resize(n);
                m_fdlImags[c].resize(n);
                for (int i = 0; i < n; ++i) {
                    m_fdlReals[c][i].assign(FFT_HALF, 0.0f);
                    m_fdlImags[c][i].assign(FFT_HALF, 0.0f);
                    m_fdl[c][i].realp = m_fdlReals[c][i].data();
                    m_fdl[c][i].imagp = m_fdlImags[c][i].data();
                }
            }

            m_irChannelCount = newIR->channelCount;
            m_activeIR = newIR;
            resetState();
        }

        m_irSwapPending.store(false, std::memory_order_release);
    }

    // State reset on re-enable
    if (m_needsStateReset.exchange(false, std::memory_order_relaxed)) {
        if (m_activeIR) resetState();
    }

    if (!m_activeIR || m_activeIR->numPartitions == 0) return;

    // If channel count changed since FDL was allocated, resize
    int numCh = channels;
    if (static_cast<int>(m_fdl.size()) < numCh) {
        int n = m_activeIR->numPartitions;
        m_fdl.resize(numCh);
        m_fdlReals.resize(numCh);
        m_fdlImags.resize(numCh);
        for (int c = static_cast<int>(m_fdl.size()) - 1; c < numCh; ++c) {
            m_fdl[c].resize(n);
            m_fdlReals[c].resize(n);
            m_fdlImags[c].resize(n);
            for (int i = 0; i < n; ++i) {
                m_fdlReals[c][i].assign(FFT_HALF, 0.0f);
                m_fdlImags[c][i].assign(FFT_HALF, 0.0f);
                m_fdl[c][i].realp = m_fdlReals[c][i].data();
                m_fdl[c][i].imagp = m_fdlImags[c][i].data();
            }
        }
    }

    int numPartitions = m_activeIR->numPartitions;
    int irCh = m_irChannelCount;
    int pos = 0;

    while (pos < frameCount) {
        // Process up to end of current partition
        int avail = std::min(frameCount - pos, PARTITION_SIZE - m_phase);

        for (int i = 0; i < avail; ++i) {
            int baseIdx = (pos + i) * numCh;

            // Save input for convolution (deinterleave)
            for (int c = 0; c < numCh; ++c) {
                m_input[c][m_phase + i] = buffer[baseIdx + c];
            }

            // Output previously convolved result (after first partition latency)
            if (m_hasOutput) {
                // Smooth fade — once per sample frame
                if (wantEnabled && hasIR && m_wetMix < 1.0f) {
                    m_wetMix = std::min(1.0f, m_wetMix + FADE_STEP);
                } else if ((!wantEnabled || !hasIR) && m_wetMix > 0.0f) {
                    m_wetMix = std::max(0.0f, m_wetMix - FADE_STEP);
                }

                float dry = 1.0f - m_wetMix;
                float wet = m_wetMix;

                for (int c = 0; c < numCh; ++c) {
                    float dryS = buffer[baseIdx + c];
                    float wetS = m_output[c][m_phase + i];
                    buffer[baseIdx + c] = dryS * dry + wetS * wet;
                }
            }
        }

        m_phase += avail;
        pos += avail;

        // When we have a full partition, convolve
        if (m_phase >= PARTITION_SIZE) {
            for (int c = 0; c < numCh; ++c) {
                // IR channel mapping:
                //   Mono IR (1ch): same IR for all audio channels
                //   Stereo IR (2ch): L/R alternating (ch % 2)
                //   Multichannel IR (N>=3): 1:1 mapping, passthrough if ch >= N
                int mappedIR;
                if (irCh == 1) {
                    mappedIR = 0;
                } else if (irCh == 2) {
                    mappedIR = c % 2;
                } else {
                    if (c < irCh) {
                        mappedIR = c;
                    } else {
                        // No IR for this channel — copy input directly to output
                        std::memcpy(m_output[c].data(), m_input[c].data(),
                                    PARTITION_SIZE * sizeof(float));
                        continue;
                    }
                }

                convolveChannel(m_activeIR->partitions[mappedIR], m_fdl[c],
                               m_fdlIdx, numPartitions,
                               m_input[c].data(), m_output[c].data(),
                               m_overlap[c].data());
            }

            m_fdlIdx = (m_fdlIdx + 1) % numPartitions;
            m_phase = 0;

            if (!m_hasOutput) {
                m_hasOutput = true;
            }
        }
    }

    // If fully faded out, clear state for clean restart
    if (m_wetMix <= 0.0f && !wantEnabled) {
        if (m_activeIR) resetState();
    }
}

#else // !__APPLE__

void ConvolutionProcessor::process(float* /*buffer*/, int /*frameCount*/, int /*channels*/)
{
    // Passthrough — vDSP not available on this platform
}

#endif // __APPLE__

// ── Self-test ───────────────────────────────────────────────────────

#ifdef __APPLE__

bool ConvolutionProcessor::selfTest()
{
    qDebug() << "[Convolution SelfTest] Starting...";
    bool allPassed = true;

    // ── Test 1: Direct convolveChannel with Dirac delta ──
    // Convolving constant 0.5 with Dirac at sample 0 should produce constant 0.5.
    {
        ConvolutionProcessor proc;

        std::vector<float> dirac(PARTITION_SIZE, 0.0f);
        dirac[0] = 1.0f;

        std::vector<std::vector<float>> irCh = { dirac };
        IRData irData;
        proc.buildIRPartitions(&irData, irCh, 48000);

        int n = irData.numPartitions;
        // Set up single-channel FDL for direct test
        proc.m_fdl.resize(1);
        proc.m_fdlReals.resize(1);
        proc.m_fdlImags.resize(1);
        proc.m_fdl[0].resize(n);
        proc.m_fdlReals[0].resize(n);
        proc.m_fdlImags[0].resize(n);
        for (int i = 0; i < n; ++i) {
            proc.m_fdlReals[0][i].assign(FFT_HALF, 0.0f);
            proc.m_fdlImags[0][i].assign(FFT_HALF, 0.0f);
            proc.m_fdl[0][i].realp = proc.m_fdlReals[0][i].data();
            proc.m_fdl[0][i].imagp = proc.m_fdlImags[0][i].data();
        }

        std::vector<float> input(PARTITION_SIZE, 0.5f);
        std::vector<float> output(PARTITION_SIZE, 0.0f);
        std::vector<float> overlap(PARTITION_SIZE, 0.0f);

        proc.convolveChannel(irData.partitions[0], proc.m_fdl[0],
                            0, n, input.data(), output.data(), overlap.data());

        float maxErr = 0;
        for (int i = 0; i < PARTITION_SIZE; ++i) {
            float diff = std::abs(output[i] - 0.5f);
            if (diff > maxErr) maxErr = diff;
        }
        float overlapMax = 0;
        for (int i = 0; i < PARTITION_SIZE; ++i) {
            float diff = std::abs(overlap[i]);
            if (diff > overlapMax) overlapMax = diff;
        }

        bool pass = (maxErr < 0.001f && overlapMax < 0.001f);
        qDebug() << "[Convolution SelfTest] Dirac passthrough:"
                 << "maxErr=" << maxErr << "overlapMax=" << overlapMax
                 << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;
    }

    // ── Test 2: Multi-block stereo pipeline ──
    // Feed 4 blocks of constant 0.5 stereo through process(). After the
    // 1-partition prefill, all output should be 0.5.
    {
        ConvolutionProcessor proc;

        std::vector<float> dirac(PARTITION_SIZE, 0.0f);
        dirac[0] = 1.0f;
        std::vector<std::vector<float>> irCh = { dirac, dirac };
        IRData irData;
        proc.buildIRPartitions(&irData, irCh, 48000);
        proc.m_activeIR = &irData;
        proc.m_irChannelCount = 2;

        int n = irData.numPartitions;
        // Set up stereo FDL
        proc.m_fdl.resize(2);
        proc.m_fdlReals.resize(2);
        proc.m_fdlImags.resize(2);
        for (int c = 0; c < 2; ++c) {
            proc.m_fdl[c].resize(n);
            proc.m_fdlReals[c].resize(n);
            proc.m_fdlImags[c].resize(n);
            for (int i = 0; i < n; ++i) {
                proc.m_fdlReals[c][i].assign(FFT_HALF, 0.0f);
                proc.m_fdlImags[c][i].assign(FFT_HALF, 0.0f);
                proc.m_fdl[c][i].realp = proc.m_fdlReals[c][i].data();
                proc.m_fdl[c][i].imagp = proc.m_fdlImags[c][i].data();
            }
        }

        proc.m_hasIR.store(true);
        proc.m_enabled.store(true);
        proc.m_needsStateReset.store(false);
        proc.m_wetMix = 1.0f;
        proc.m_hasOutput = true;
        proc.m_phase = 0;

        // Process 4 separate blocks (simulating real audio callbacks)
        const int numBlocks = 4;
        float maxErr = 0;
        for (int b = 0; b < numBlocks; ++b) {
            std::vector<float> block(PARTITION_SIZE * 2, 0.5f);  // stereo interleaved
            proc.process(block.data(), PARTITION_SIZE, 2);

            // After first block, output should start appearing
            if (b >= 1) {
                for (int i = 0; i < PARTITION_SIZE; ++i) {
                    float diffL = std::abs(block[i * 2] - 0.5f);
                    float diffR = std::abs(block[i * 2 + 1] - 0.5f);
                    if (diffL > maxErr) maxErr = diffL;
                    if (diffR > maxErr) maxErr = diffR;
                }
            }
        }

        bool pass = (maxErr < 0.001f);
        qDebug() << "[Convolution SelfTest] Pipeline passthrough:"
                 << "maxErr=" << maxErr << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;

        proc.m_activeIR = nullptr;
    }

    // ── Test 3: Mono IR applied to stereo ──
    {
        ConvolutionProcessor proc;

        std::vector<float> dirac(PARTITION_SIZE, 0.0f);
        dirac[0] = 1.0f;
        std::vector<std::vector<float>> irCh = { dirac };  // mono IR
        IRData irData;
        proc.buildIRPartitions(&irData, irCh, 48000);
        proc.m_activeIR = &irData;
        proc.m_irChannelCount = 1;

        int n = irData.numPartitions;
        proc.m_fdl.resize(2);
        proc.m_fdlReals.resize(2);
        proc.m_fdlImags.resize(2);
        for (int c = 0; c < 2; ++c) {
            proc.m_fdl[c].resize(n);
            proc.m_fdlReals[c].resize(n);
            proc.m_fdlImags[c].resize(n);
            for (int i = 0; i < n; ++i) {
                proc.m_fdlReals[c][i].assign(FFT_HALF, 0.0f);
                proc.m_fdlImags[c][i].assign(FFT_HALF, 0.0f);
                proc.m_fdl[c][i].realp = proc.m_fdlReals[c][i].data();
                proc.m_fdl[c][i].imagp = proc.m_fdlImags[c][i].data();
            }
        }

        proc.m_hasIR.store(true);
        proc.m_enabled.store(true);
        proc.m_needsStateReset.store(false);
        proc.m_wetMix = 1.0f;
        proc.m_hasOutput = true;
        proc.m_phase = 0;

        const int numBlocks = 4;
        float maxErr = 0;
        for (int b = 0; b < numBlocks; ++b) {
            std::vector<float> block(PARTITION_SIZE * 2, 0.5f);
            proc.process(block.data(), PARTITION_SIZE, 2);
            if (b >= 1) {
                for (int i = 0; i < PARTITION_SIZE; ++i) {
                    float diffL = std::abs(block[i * 2] - 0.5f);
                    float diffR = std::abs(block[i * 2 + 1] - 0.5f);
                    if (diffL > maxErr) maxErr = diffL;
                    if (diffR > maxErr) maxErr = diffR;
                }
            }
        }

        bool pass = (maxErr < 0.001f);
        qDebug() << "[Convolution SelfTest] Mono IR → stereo:"
                 << "maxErr=" << maxErr << (pass ? "PASS" : "FAIL");
        if (!pass) allPassed = false;

        proc.m_activeIR = nullptr;
    }

    qDebug() << "[Convolution SelfTest]" << (allPassed ? "ALL PASSED" : "SOME FAILED");
    return allPassed;
}

#else // !__APPLE__

bool ConvolutionProcessor::selfTest()
{
    qDebug() << "[Convolution SelfTest] Skipped — vDSP not available";
    return true;
}

#endif // __APPLE__
