#include "fitshowcontroller.h"
#include <algorithm>
namespace Treadmill {
FitShowController::FitShowController(FitShow::Control &control, DeviceProfile profile,
                                   std::function<qint64()> receiveClock, std::function<void()> open,
                                   std::function<void()> close, QObject *parent)
    : Controller(parent), control_(control), profile_(std::move(profile)), receiveClock_(std::move(receiveClock)),
      open_(std::move(open)), close_(std::move(close)) {
    connect(&control_, &FitShow::Control::commandEvent, this, &Controller::commandEvent);
    connect(&control_, &FitShow::Control::fault, this, [this](const QString &reason) {
        faulted_ = true;
        emit fault(reason);
    });
    connect(&control_, &FitShow::Control::connectionChanged, this, [this](bool connected) {
        if (!connected) { boundsMatch_ = identityMatch_ = pendingStatus_ = hasStatus_ = false; }
        emit connectionChanged(connected);
        if (connected) {
            control_.allowDiagnostics(profile_.diagnosticsConfirmed);
            if (profile_.diagnosticsConfirmed) control_.queryLimits(0);
        }
    });
    connect(&control_, &FitShow::Control::limitsChanged, this, [this](FitShow::Limits limits) {
        if (!limits.complete()) return;
        boundsMatch_ = profile_.matches(limits);
        if (!boundsMatch_ && profile_.motorEvidenceComplete()) { faulted_ = true; control_.cancel(); emit fault("Live FitShow bounds/unit tag differ from the configured profile"); }
    });
    connect(&control_, &FitShow::Control::statusChanged, this, [this](FitShow::Status status) {
        if (!status.hasMotorData) return;
        status_ = status;
        if (status.state == 10 && status.speedTenths == 0) startPending_ = false;
        arrivedAtMs_ = receiveClock_();
        pendingStatus_ = hasStatus_ = true;
    });
}
Capabilities FitShowController::capabilities() const {
    auto result = profile_.capabilities();
    if (control_.transportAvailable()) result.pauseSupported = result.pauseSupported && control_.limits().pause;
    return result;
}
void FitShowController::connectDevice(quint64 generation) {
    const auto error = profile_.validate();
    if (!error.isEmpty() || !profile_.diagnosticsConfirmed) {
        emit fault(error.isEmpty() ? "Nonmotor diagnostics require explicit approval" : error);
        return;
    }
    generation_ = generation;
    faulted_ = startPending_ = false;
    boundsMatch_ = identityMatch_ = pendingStatus_ = hasStatus_ = false;
    open_();
}
void FitShowController::disconnectDevice() { close_(); }
void FitShowController::observeIdentity(const QString &model, const QString &firmware) {
    emit identityObserved(model, firmware);
    identityMatch_ = !model.isEmpty() && !firmware.isEmpty() && model == profile_.model && firmware == profile_.firmware;
    if (!identityMatch_ && profile_.motorEvidenceComplete()) {
        faulted_ = true;
        control_.cancel();
        emit fault("Live model/firmware identity is missing or differs from the verified profile");
    }
}
bool FitShowController::canControl(quint64 generation, quint64 id, Target target, int &speed, int &incline) {
    if (faulted_ || generation != generation_ || !profile_.motorEvidenceComplete() || !boundsMatch_ || !identityMatch_ ||
        !control_.transportAvailable() || !profile_.toWire(target, speed, incline)) {
        emit commandEvent(id, "rejected"); return false;
    }
    return true;
}
void FitShowController::start(quint64 generation, quint64 id, Target initial) {
    int speed = 0, incline = 0;
    if (!canControl(generation, id, initial, speed, incline)) return;
    const qint64 age = receiveClock_() - arrivedAtMs_;
    if (startPending_ || !hasStatus_ || age < 0 || age > 2000 || status_.speedTenths != 0 ||
        (status_.state != 1 && status_.state != 4 && status_.state != 10) || !control_.arm(generation)) {
        emit commandEvent(id, "rejected"); return;
    }
    startPending_ = true;
    control_.start(generation, id, speed, incline);
}
void FitShowController::setTarget(quint64 generation, quint64 id, Target target) {
    int speed = 0, incline = 0;
    if (!hasStatus_ || status_.state != 3 || receiveClock_() - arrivedAtMs_ < 0 || receiveClock_() - arrivedAtMs_ > 2000) { emit commandEvent(id, "rejected"); return; }
    if (canControl(generation, id, target, speed, incline)) control_.target(generation, id, speed, incline);
}
void FitShowController::stop(quint64 generation, quint64 id) {
    generation_ = generation;
    startPending_ = false;
    control_.stop(generation, id);
}
void FitShowController::pause(quint64 generation, quint64 id) {
    if (!hasStatus_ || status_.state != 3 || receiveClock_() - arrivedAtMs_ < 0 || receiveClock_() - arrivedAtMs_ > 2000 || !profile_.pauseVerified || !identityMatch_ || !boundsMatch_ || generation != generation_) { emit commandEvent(id, "rejected"); return; }
    control_.pause(generation, id);
}
void FitShowController::advance(qint64 nowMs) {
    if (faulted_) return;
    if (hasStatus_ && profile_.motorEvidenceComplete() && control_.transportAvailable() &&
        (receiveClock_() - arrivedAtMs_ < 0 || receiveClock_() - arrivedAtMs_ > 2000)) {
        faulted_ = true;
        control_.cancel();
        emit fault("Raw FitShow motor report is stale or clock changed");
        return;
    }
    if (!pendingStatus_) return;
    pendingStatus_ = false;
    if (!boundsMatch_ || !identityMatch_ || !profile_.motorEvidenceComplete() || !control_.transportAvailable()) return;
    const qint64 age = receiveClock_() - arrivedAtMs_;
    if (age < 0 || age > 2000) { faulted_ = true; control_.cancel(); emit fault("Raw FitShow motor report is stale or clock changed"); return; }
    Target actual;
    if (!profile_.fromWire(status_.speedTenths, status_.inclineCode, actual)) {
        faulted_ = true; control_.cancel(); emit fault("Unmapped/out-of-range raw FitShow telemetry"); return;
    }
    Telemetry telemetry;
    telemetry.atMs = nowMs - age;
    telemetry.connected = true;
    telemetry.running = status_.state == 3;
    telemetry.paused = status_.state == 10;
    telemetry.actual = actual;
    emit sample(telemetry); // Exactly once for a new, complete motor report.
}
}
