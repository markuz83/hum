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

Analyzer::Analyzer(int bands, int framerate)
    : m_bands(bands), m_framerate(framerate), m_levels(bands, 0.0f), m_window(FftSize),
      m_peak(bands, 0.0f), m_fall(bands, 0.0f), m_previous(bands, 0.0f), m_memory(bands, 0.0f) {
    for (int i = 0; i < FftSize; ++i)
        m_window[i] = 0.5f - 0.5f * std::cos(2.0f * float(M_PI) * i / (FftSize - 1));
}

double Analyzer::bandFrequency(int band) const {
    return LowestHz * std::pow(HighestHz / LowestHz, double(band) / m_bands);
}

// A new track starts from silence, but keeps the learned sensitivity so the
// bars don't have to find their scale again.
void Analyzer::clear() {
    m_history.clear();
    m_levels.fill(0.0f);
    m_peak.fill(0.0f);
    m_fall.fill(0.0f);
    m_previous.fill(0.0f);
    m_memory.fill(0.0f);
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
    // that puts a full-scale tone at 1.
    const float norm = FftSize / 4.0f;
    const int bins = FftSize / 2;
    auto binOf = [&](double hz) { return std::clamp(int(std::lround(hz * FftSize / m_rate)), 1, bins - 1); };

    QVector<float> raw(m_bands);
    for (int band = 0; band < m_bands; ++band) {
        const int low = binOf(bandFrequency(band));
        const int high = std::max(low, binOf(bandFrequency(band + 1)) - 1);
        float sum = 0;
        for (int bin = low; bin <= high; ++bin)
            sum += std::abs(data[bin]) / norm;
        // cava equalizes by frequency, because music carries far less energy
        // up high, and averages over the bins a bar spans.
        const double center = std::sqrt(bandFrequency(band) * bandFrequency(band + 1));
        raw[band] = sum / (high - low + 1) * float(center / 1000.0);
    }
    return smooth(raw);
}

QVector<float> Analyzer::decay() {
    return smooth(QVector<float>(m_bands, 0.0f));
}

// The smoothing stages of cava's cavacore, in its order.
QVector<float> Analyzer::smooth(const QVector<float> &raw) {
    // Gravity scales with the frame rate so the fall looks the same at any speed.
    constexpr float noiseReduction = 0.77f;
    const float gravity = std::pow(60.0f / m_framerate, 2.5f) * 1.54f / noiseReduction;

    bool silence = true, overshoot = false;
    for (int n = 0; n < m_bands; ++n) {
        float out = raw[n] * m_sensitivity;
        if (raw[n] > 1e-4f)
            silence = false;

        // Gravity: a bar that drops starts from its peak and falls faster and
        // faster; a bar that rises follows the signal at once.
        if (out < m_previous[n]) {
            out = std::max(0.0f, m_peak[n] * (1.0f - m_fall[n] * m_fall[n] * gravity));
            m_fall[n] += 0.028f;
        } else {
            m_peak[n] = out;
            m_fall[n] = 0.0f;
        }
        m_previous[n] = out;

        // Integral: a running sum that fades by the noise-reduction factor,
        // which smooths out frame-to-frame flicker.
        m_memory[n] = m_memory[n] * noiseReduction + out;
        out = m_memory[n] < 0.005f ? 0.0f : m_memory[n];

        if (out > 1.0f) {
            overshoot = true;
            out = 1.0f;
        }
        m_levels[n] = out;
    }

    // Autosensitivity: back off when a bar hits the top, creep up otherwise,
    // and climb quickly at the start until the first overshoot finds the scale.
    if (overshoot) {
        m_sensitivity *= 0.98f;
        m_sensitivityInit = false;
    } else if (!silence) {
        m_sensitivity *= 1.001f;
        if (m_sensitivityInit)
            m_sensitivity *= 1.1f;
    }
    return m_levels;
}
