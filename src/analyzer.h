#pragma once

#include <QAudioBuffer>
#include <QVector>

// Turns the audio that is playing right now into a handful of bar heights.
// It keeps the most recent samples, runs an FFT over them, groups the bins
// into log-spaced bands (the ear hears pitch logarithmically), and smooths
// the result so bars rise fast and fall slowly like a classic spectrum meter.
class Analyzer {
public:
    static constexpr int FftSize = 2048;

    explicit Analyzer(int bands = 48);

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
    int m_bands;
    int m_rate = 44100;
    QVector<float> m_history;
    QVector<float> m_levels;
    QVector<float> m_window;
};
