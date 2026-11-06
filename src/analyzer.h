#pragma once

#include <QAudioBuffer>
#include <QVector>

// Turns the audio that is playing right now into a handful of bar heights.
// It keeps the most recent samples, runs an FFT over them, and groups the bins
// into log-spaced bands (the ear hears pitch logarithmically). The smoothing
// follows cava's cavacore (MIT, Karl Stavestrand): bars fall under gravity
// that accelerates, an integral filter averages recent frames, and an
// automatic sensitivity keeps the loudest bars just under the top.
class Analyzer {
public:
    static constexpr int FftSize = 2048;

    explicit Analyzer(int bands = 48, int framerate = 30);

    int bands() const { return m_bands; }
    // Lower edge of a band in Hz, so tests and callers can reason about pitch.
    double bandFrequency(int band) const;

    void clear();
    void push(const QAudioBuffer &buffer);
    void push(const float *mono, int frames, int sampleRate);
    // Levels from 0 to 1 for the samples pushed so far.
    QVector<float> compute();
    // Lets the bars fall back toward zero when no new audio arrives.
    QVector<float> decay();

private:
    QVector<float> smooth(const QVector<float> &raw);

    int m_bands;
    int m_framerate;
    int m_rate = 44100;
    QVector<float> m_history;
    QVector<float> m_levels;
    QVector<float> m_window;
    // Per-bar smoothing state, as in cava: the peak a falling bar started from,
    // how long it has been falling, its last output, and the integral memory.
    QVector<float> m_peak, m_fall, m_previous, m_memory;
    float m_sensitivity = 1.0f;
    bool m_sensitivityInit = true;
};
