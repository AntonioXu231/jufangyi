#include "demo_source.h"

#include <QRandomGenerator>

#include <cmath>

namespace {
constexpr int kChannels = 4;
constexpr int kSamples = 1024;
constexpr double kPi = 3.14159265358979323846;
}

DemoSource::DemoSource(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(20);                 // One synthetic 50 Hz cycle per frame
    connect(&m_timer, &QTimer::timeout, this, &DemoSource::makeFrame);
}

void DemoSource::start() { m_timer.start(); }
void DemoSource::stop() { m_timer.stop(); }
bool DemoSource::isRunning() const { return m_timer.isActive(); }

void DemoSource::makeFrame()
{
    ScopeFrame frame;
    frame.sequence = m_sequence++;
    frame.phaseSynchronized = true; // One demo frame intentionally spans one 360-degree cycle.
    frame.samples.resize(kChannels);
    for (int channel = 0; channel < kChannels; ++channel) {
        auto &data = frame.samples[channel];
        data.resize(kSamples);
        const double channelPhase = channel * 0.57 + frame.sequence * 0.028;
        const int pulseCentre = (static_cast<int>(frame.sequence * 37U + channel * 211U) % 900) + 60;
        const bool injectPulse = ((frame.sequence + static_cast<quint64>(channel * 3)) % 17U) == 0U;
        for (int i = 0; i < kSamples; ++i) {
            const double phase = 2.0 * kPi * i / (kSamples - 1) + channelPhase;
            const double noise = QRandomGenerator::global()->bounded(-38, 39);
            double value = 470.0 * std::sin(phase) + 80.0 * std::sin(phase * 7.0) + noise;
            if (injectPulse) {
                const double d = static_cast<double>(i - pulseCentre);
                value += 1500.0 * std::exp(-(d * d) / 9.0);
            }
            data[i] = static_cast<qint16>(qBound(-2047.0, value, 2047.0));
        }
    }
    emit frameReady(frame);
}
