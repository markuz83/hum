#include "analyzer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace {
constexpr double LowestHz = 40;
constexpr double HighestHz = 16000;

// Iterative radix-2 FFT; the size is a power of two by construction.
void fft(std::vector<std::complex<float>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float angle = -2.0f * float(M_PI) / float(len);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}
}

Analyzer::Analyzer(int bands) : m_bands(bands), m_levels(bands, 0.0f), m_window(FftSize) {
    for (int i = 0; i < FftSize; ++i)
        m_window[i] = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (FftSize - 1));
}

double Analyzer::bandFrequency(int band) const {
    return LowestHz * std::pow(HighestHz / LowestHz, double(band) / m_bands);
}

void Analyzer::clear() {
    m_history.clear();
    m_levels.fill(0.0f);
}

void Analyzer::push(const QAudioBuffer &buffer) {
    if (!buffer.isValid())
        return;
    const auto format = buffer.format();
    const int channels = std::max(1, format.channelCount()), frames = buffer.frameCount();
    QVector<float> mono(frames, 0.0f);

    // Whatever the decoder hands over becomes -1..1, averaged across channels.
    auto fill = [&](const auto *samples, float scale, float offset) {
        for (int f = 0; f < frames; ++f) {
            float sum = 0;
            for (int c = 0; c < channels; ++c)
                sum += (float(samples[f * channels + c]) - offset) * scale;
            mono[f] = sum / channels;
        }
    };
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8: fill(buffer.constData<quint8>(), 1.0f / 128, 128); break;
    case QAudioFormat::Int16: fill(buffer.constData<qint16>(), 1.0f / 32768, 0); break;
    case QAudioFormat::Int32: fill(buffer.constData<qint32>(), 1.0f / 2147483648.0f, 0); break;
    case QAudioFormat::Float: fill(buffer.constData<float>(), 1.0f, 0); break;
    default: return;
    }
    push(mono.constData(), frames, format.sampleRate());
}

void Analyzer::push(const float *mono, int frames, int sampleRate) {
    if (sampleRate > 0)
        m_rate = sampleRate;
    m_history.append(QVector<float>(mono, mono + frames));
    if (m_history.size() > FftSize)
        m_history.remove(0, m_history.size() - FftSize);
}

QVector<float> Analyzer::compute() {
    std::vector<std::complex<float>> data(FftSize);
    const int offset = FftSize - int(m_history.size());
    for (int i = 0; i < m_history.size(); ++i)
        data[offset + i] = m_history[i] * m_window[offset + i];
    fft(data);

    // A full-scale sine shows up as N/4 after a Hann window, so dividing by
    // that puts 0 dB at full scale.
    const float norm = FftSize / 4.0f;
    const int bins = FftSize / 2;
    auto binOf = [&](double hz) { return std::clamp(int(std::lround(hz * FftSize / m_rate)), 1, bins - 1); };

    for (int band = 0; band < m_bands; ++band) {
        const int low = binOf(bandFrequency(band));
        const int high = std::max(low, binOf(bandFrequency(band + 1)) - 1);
        float peak = 0;
        for (int bin = low; bin <= high; ++bin)
            peak = std::max(peak, std::abs(data[bin]) / norm);
        // Music has far less energy up high, so tilt the meter to even it out.
        const float tilt = 14.0f * band / m_bands;
        const float decibels = 20.0f * std::log10(peak + 1e-7f);
        const float target = std::clamp((decibels + tilt + 72.0f) / 62.0f, 0.0f, 1.0f);
        float &level = m_levels[band];
        level = target > level ? level + (target - level) * 0.7f : level - (level - target) * 0.2f;
    }
    return m_levels;
}

QVector<float> Analyzer::decay() {
    for (float &level : m_levels)
        level = level < 0.005f ? 0.0f : level * 0.85f;
    return m_levels;
}
