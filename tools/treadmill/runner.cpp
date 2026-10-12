#include "runner.h"
#include <algorithm>
namespace Treadmill {
Runner::Runner(Controller &controller, Workout workout, QObject *parent)
    : QObject(parent), controller_(controller), workout_(std::move(workout)) {
    connect(&controller_, &Controller::sample, this, &Runner::onSample);
    connect(&controller_, &Controller::fault, this, [this](const QString &reason) {
        if (!finished()) abort(reason);
    });
    connect(&controller_, &Controller::commandEvent, this, [this](quint64 id, const QString &phase) {
        emit logEvent("command", QString("%1 %2").arg(id).arg(phase));
        if (id == latestId_ && phase == "rejected" && !finished() && state_ != State::Stopping)
            abort("Controller rejected command");
    });
}
QString Runner::stateName(State state) {
    switch (state) {
    case State::Disconnected: return "DISCONNECTED";
    case State::Preflight: return "PREFLIGHT";
    case State::Ready: return "READY";
    case State::Starting: return "STARTING";
    case State::Running: return "RUNNING";
    case State::Pausing: return "PAUSING";
    case State::Paused: return "PAUSED";
    case State::Stopping: return "STOPPING";
    case State::Completed: return "COMPLETED";
    case State::Aborted: return "ABORTED";
    case State::StopUnconfirmed: return "STOP_UNCONFIRMED";
    case State::Fault: return "FAULT";
    }
    return "UNKNOWN";
}
void Runner::setState(State state) {
    state_ = state;
    emit stateChanged(stateName(state));
}
bool Runner::finished() const {
    return state_ == State::Completed || state_ == State::Aborted ||
           state_ == State::StopUnconfirmed || state_ == State::Fault;
}
void Runner::prepare(qint64 nowMs) {
    if (state_ != State::Disconnected) return;
    nowMs_ = nowMs;
    lastAdvanceMs_ = nowMs;
    const QString error = validateWorkout(workout_, controller_.capabilities());
    if (!error.isEmpty()) {
        reason_ = error;
        setState(State::Fault);
        return;
    }
    setState(State::Preflight);
    deadlineMs_ = nowMs_ + 10000;
    controller_.connectDevice(generation_);
}
void Runner::confirmStart() {
    if (state_ != State::Ready || !telemetry_.connected || nowMs_ - telemetry_.atMs > 2000) return;
    requested_ = quantize(workout_.stages[0].from);
    deadlineMs_ = nowMs_ + 15000;
    targetSamples_ = 0;
    setState(State::Starting);
    latestId_ = ++nextId_;
    controller_.start(generation_, latestId_, requested_);
}
Target Runner::quantize(Target target) const {
    const auto caps = controller_.capabilities();
    target.speedKmh = std::min(target.speedKmh, speedCeiling_);
    target.gradePercent = std::min(target.gradePercent, gradeCeiling_);
    // Round down so adaptation never raises the requested intensity.
    target.speedKmh = caps.minSpeedKmh + std::floor((target.speedKmh - caps.minSpeedKmh + 1e-8) / caps.speedStepKmh) * caps.speedStepKmh;
    target.gradePercent = caps.minGradePercent + std::floor((target.gradePercent - caps.minGradePercent + 1e-8) / caps.gradeStepPercent) * caps.gradeStepPercent;
    return target;
}
bool Runner::observed(Target target) const {
    return telemetry_.connected && telemetry_.running &&
           std::abs(telemetry_.actual.speedKmh - target.speedKmh) < 0.051 &&
           std::abs(telemetry_.actual.gradePercent - target.gradePercent) < 0.051;
}
void Runner::onSample(Telemetry sample) {
    if (sample.atMs < 0 || sample.atMs > nowMs_ || sample.atMs <= telemetry_.atMs ||
        !std::isfinite(sample.actual.speedKmh) || !std::isfinite(sample.actual.gradePercent)) return;
    telemetry_ = sample;
    emit logEvent("telemetry", QString("%1 km/h %2% running=%3").arg(sample.actual.speedKmh).arg(sample.actual.gradePercent).arg(sample.running));
    if (!sample.connected) {
        abort("Connection lost");
        return;
    }
    if (state_ == State::Preflight) {
        if (sample.running || sample.actual.speedKmh > 0.01) {
            reason_ = "Initial treadmill state is moving";
            setState(State::Fault);
            return;
        }
        if (++stoppedSamples_ >= 3) setState(State::Ready);
    } else if (state_ == State::Stopping || state_ == State::Pausing) {
        if (!sample.running && sample.actual.speedKmh < 0.01) ++stoppedSamples_;
        else stoppedSamples_ = 0;
        if (stoppedSamples_ >= 3) {
            if (state_ == State::Pausing && sample.paused) setState(State::Paused);
            else if (state_ == State::Stopping) setState(normalStop_ ? State::Completed : State::Aborted);
        }
    } else if (state_ == State::Starting) {
        if (observed(requested_)) ++targetSamples_;
        else targetSamples_ = 0;
        if (targetSamples_ >= 3) {
            setState(State::Running);
            targetSinceMs_ = nowMs_;
            enterStage();
        }
    } else if (state_ == State::Running) {
        if (!sample.running) {
            abort("Unexpected physical stop/running-state change");
            return;
        }
        if (observed(requested_)) {
            ++targetSamples_;
            if (targetSamples_ == 3) emit logEvent("command", QString("%1 settled").arg(latestId_));
        } else {
            if (targetSamples_ >= 3) {
                abort("Manual override or unexpected target change");
                return;
            }
            targetSamples_ = 0;
        }
    }
}
void Runner::requestTarget(Target target) {
    if (state_ != State::Running) return;
    target = quantize(target);
    if (!controller_.capabilities().accepts(target)) { abort("Invalid mapped target"); return; }
    if (std::abs(target.speedKmh - requested_.speedKmh) < 1e-8 &&
        std::abs(target.gradePercent - requested_.gradePercent) < 1e-8) return;
    requested_ = target;
    targetSinceMs_ = nowMs_;
    targetSamples_ = 0;
    latestId_ = ++nextId_;
    emit logEvent("target", QString("%1 km/h %2%").arg(target.speedKmh).arg(target.gradePercent));
    controller_.setTarget(generation_, latestId_, target);
}
void Runner::enterStage() {
    const auto &stage = workout_.stages[stageIndex_];
    emit logEvent("stage", stage.name);
    if (!stage.instruction.isEmpty()) emit logEvent("instruction", stage.instruction);
    requestTarget(stage.targetAt(stageElapsedMs_));
}
qint64 Runner::elapsedMs() const {
    qint64 elapsed = stageElapsedMs_;
    for (int i = 0; i < stageIndex_; ++i) elapsed += workout_.stages[i].durationMs;
    return elapsed;
}
void Runner::stop(bool normal, const QString &reason) {
    if (state_ == State::Stopping || finished()) return;
    normalStop_ = normal;
    reason_ = reason;
    ++generation_;
    stoppedSamples_ = 0;
    setState(State::Stopping);
    deadlineMs_ = nowMs_ + 10000;
    latestId_ = ++nextId_;
    controller_.stop(generation_, latestId_);
}
void Runner::abort(const QString &reason) {
    if (finished()) return;
    if (state_ == State::Stopping) {
        normalStop_ = false;
        reason_ = reason;
        return;
    }
    stop(false, reason);
}
void Runner::pause() {
    if (state_ != State::Running || !controller_.capabilities().pauseSupported) return;
    setState(State::Pausing);
    stoppedSamples_ = 0;
    deadlineMs_ = nowMs_ + 10000;
    latestId_ = ++nextId_;
    controller_.pause(generation_, latestId_);
}
void Runner::resume() {
    if (state_ != State::Paused || nowMs_ - telemetry_.atMs > 2000) return;
    requested_ = quantize(workout_.stages[stageIndex_].targetAt(stageElapsedMs_));
    targetSamples_ = 0;
    deadlineMs_ = nowMs_ + 15000;
    setState(State::Starting);
    latestId_ = ++nextId_;
    controller_.start(generation_, latestId_, requested_);
}
void Runner::reduce() {
    if (state_ != State::Running && state_ != State::Paused) return;
    const auto caps = controller_.capabilities();
    speedCeiling_ = std::max(caps.minSpeedKmh, requested_.speedKmh - caps.speedStepKmh);
    gradeCeiling_ = std::max(caps.minGradePercent, requested_.gradePercent - caps.gradeStepPercent);
    emit logEvent("user", "Intensity reduction latched");
    requestTarget({speedCeiling_, gradeCeiling_});
}
void Runner::advance(qint64 nowMs) {
    if (finished() || state_ == State::Disconnected) return;
    if (nowMs < lastAdvanceMs_) { abort("Monotonic clock moved backwards"); return; }
    const qint64 delta = nowMs - lastAdvanceMs_;
    const bool wasRunning = state_ == State::Running;
    nowMs_ = nowMs;
    lastAdvanceMs_ = nowMs;
    if (delta > 1500 && state_ != State::Stopping) abort("Control loop gap exceeded 1.5 seconds");
    controller_.advance(nowMs_);
    if (finished()) return;
    if (telemetry_.atMs < 0 || nowMs_ - telemetry_.atMs > 2000) {
        if (state_ != State::Stopping) abort("Motor telemetry stale");
    }
    if ((state_ == State::Preflight || state_ == State::Starting || state_ == State::Pausing || state_ == State::Stopping) &&
        nowMs_ >= deadlineMs_) {
        if (state_ == State::Stopping) {
            reason_ += "; stop unconfirmed — use the physical stop mechanism";
            setState(State::StopUnconfirmed);
        } else abort("State deadline exceeded");
        return;
    }
    if (state_ != State::Running) return;
    if (nowMs_ - targetSinceMs_ > 30000 && !observed(requested_)) { abort("Target settling timeout"); return; }
    const auto &stage = workout_.stages[stageIndex_];
    if (!stageEnding_ && wasRunning) stageElapsedMs_ = std::min(stage.durationMs, stageElapsedMs_ + delta);
    requestTarget(stage.targetAt(stageElapsedMs_));
    if (state_ != State::Running) return;
    const qint64 remaining = stage.durationMs - stageElapsedMs_;
    if (stageIndex_ + 1 < workout_.stages.size()) {
        const auto &next = workout_.stages[stageIndex_ + 1];
        for (const qint64 lead : {30000LL, 10000LL}) {
            const QString key = QString("%1:lead:%2").arg(stageIndex_).arg(lead);
            if (stage.durationMs >= lead && remaining <= lead && !notified_.contains(key)) {
                notified_.insert(key);
                const auto pair = quantize(next.from);
                emit logEvent("notification", QString("Next in %1 s: %2, %3 km/h, %4%")
                           .arg(remaining / 1000).arg(next.name).arg(pair.speedKmh).arg(pair.gradePercent));
            }
        }
    }
    for (int i = 0; i < stage.events.size(); ++i) {
        const auto &text = stage.events[i];
        const QString key = QString("%1:text:%2").arg(stageIndex_).arg(i);
        if (stageElapsedMs_ >= text.offsetMs && !notified_.contains(key)) {
            notified_.insert(key);
            emit logEvent("notification", text.message);
        }
    }
    if (remaining == 0) {
        stageEnding_ = true;
        if (targetSamples_ < 3) return;
        if (stageIndex_ + 1 == workout_.stages.size()) stop(true, "Workout complete");
        else {
            ++stageIndex_;
            stageElapsedMs_ = 0;
            stageEnding_ = false;
            enterStage();
        }
    }
}
}
