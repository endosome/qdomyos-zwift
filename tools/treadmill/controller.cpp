#include "controller.h"
#include <algorithm>

namespace Treadmill {
bool Capabilities::accepts(Target target) const {
    if (!validated || !std::isfinite(target.speedKmh) || !std::isfinite(target.gradePercent) ||
        !std::isfinite(minSpeedKmh) || !std::isfinite(maxSpeedKmh) ||
        !std::isfinite(minGradePercent) || !std::isfinite(maxGradePercent) ||
        !std::isfinite(speedStepKmh) || !std::isfinite(gradeStepPercent) ||
        minSpeedKmh <= 0 || maxSpeedKmh < minSpeedKmh || speedStepKmh <= 0 ||
        minGradePercent < 0 || maxGradePercent < minGradePercent || gradeStepPercent <= 0)
        return false;
    return target.speedKmh >= minSpeedKmh - 1e-8 && target.speedKmh <= maxSpeedKmh + 1e-8 &&
           target.gradePercent >= minGradePercent - 1e-8 && target.gradePercent <= maxGradePercent + 1e-8;
}
Simulator::Simulator(QObject *parent) : Controller(parent) {}
Capabilities Simulator::capabilities() const {
    // Explicit simulator model; these numbers are not W1 specifications.
    return {true, true, 0.5, 6.0, 0.1, 0.0, 10.0, 1.0};
}
void Simulator::connectDevice(quint64 generation) {
    generation_ = generation;
    telemetry_ = {};
    telemetry_.connected = true;
    telemetry_.atMs = nowMs_;
    target_ = {};
    startAtMs_ = -1;
    stopping_ = false;
    pausePending_ = false;
    acknowledgements_.clear();
    emit sample(telemetry_);
}
bool Simulator::accept(quint64 generation, quint64 id, Target target) {
    if (generation != generation_ || !telemetry_.connected || stopping_ || !capabilities().accepts(target)) {
        emit commandEvent(id, "rejected");
        return false;
    }
    return true;
}
void Simulator::submit(quint64 id) {
    emit commandEvent(id, "submitted");
    acknowledgements_.append({id, nowMs_ + 100});
}
void Simulator::start(quint64 generation, quint64 id, Target initial) {
    if (!accept(generation, id, initial) || telemetry_.running || telemetry_.actual.speedKmh > 0) {
        emit commandEvent(id, "rejected");
        return;
    }
    target_ = initial;
    telemetry_.paused = false;
    pausePending_ = false;
    startAtMs_ = nowMs_ + 2000;
    submit(id);
}
void Simulator::setTarget(quint64 generation, quint64 id, Target target) {
    if (!accept(generation, id, target)) return;
    if (!telemetry_.running || failure_ == Failure::RejectTarget) {
        emit commandEvent(id, "rejected");
        return;
    }
    target_ = target;
    submit(id);
}
void Simulator::stop(quint64 generation, quint64 id) {
    if (generation < generation_ || !telemetry_.connected) {
        emit commandEvent(id, "rejected");
        return;
    }
    generation_ = generation;
    acknowledgements_.clear();
    startAtMs_ = -1;
    pausePending_ = false;
    stopping_ = true;
    submit(id);
    if (failure_ != Failure::StopFailure) {
        target_.speedKmh = 0;
        target_.gradePercent = telemetry_.actual.gradePercent;
    }
}
void Simulator::pause(quint64 generation, quint64 id) {
    if (generation != generation_ || !telemetry_.running || stopping_) {
        emit commandEvent(id, "rejected");
        return;
    }
    target_.speedKmh = 0;
    pausePending_ = true;
    submit(id);
}
void Simulator::advance(qint64 nowMs) {
    if (nowMs < nowMs_) {
        emit fault("Simulator clock moved backwards");
        return;
    }
    const double seconds = (nowMs - nowMs_) / 1000.0;
    nowMs_ = nowMs;
    if (!telemetry_.connected) return;
    if (failure_ == Failure::Disconnect) {
        telemetry_.connected = false;
        acknowledgements_.clear();
        startAtMs_ = -1;
        emit fault("Simulated connection loss");
        return;
    }
    for (int i = acknowledgements_.size() - 1; i >= 0; --i) {
        if (acknowledgements_[i].dueMs <= nowMs_) {
            const auto id = acknowledgements_.takeAt(i).id;
            if (failure_ != Failure::NoAcknowledgements) emit commandEvent(id, "acknowledged");
        }
    }
    if (startAtMs_ >= 0 && nowMs_ >= startAtMs_) {
        telemetry_.running = true;
        startAtMs_ = -1;
    }
    auto approach = [seconds](double actual, double target, double rate) {
        const double delta = std::clamp(target - actual, -rate * seconds, rate * seconds);
        return actual + delta;
    };
    if (telemetry_.running) {
        telemetry_.actual.speedKmh = approach(telemetry_.actual.speedKmh, target_.speedKmh, 0.5);
        telemetry_.actual.gradePercent = approach(telemetry_.actual.gradePercent, target_.gradePercent, 1.0);
    }
    if (telemetry_.actual.speedKmh <= 1e-8 && (stopping_ || pausePending_)) {
        telemetry_.actual.speedKmh = 0;
        telemetry_.running = false;
        telemetry_.paused = pausePending_;
    }
    if (failure_ == Failure::ManualOverride && telemetry_.running) {
        telemetry_.actual.speedKmh = std::max(0.5, telemetry_.actual.speedKmh - 1.0);
        target_.speedKmh = telemetry_.actual.speedKmh;
        failure_ = Failure::None; // One console intervention, not resistance to every stop.
    }
    if (failure_ != Failure::StaleTelemetry && nowMs_ - lastSampleMs_ >= 200) {
        telemetry_.atMs = nowMs_;
        lastSampleMs_ = nowMs_;
        emit sample(telemetry_);
    }
}
}
