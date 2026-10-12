#pragma once
#include "profile.h"
#include <functional>
namespace Treadmill {
// Backend-independent adapter over the same restricted queue used by the QZ driver.
// open/close are transport lifecycle hooks; clocks are injected for offline tests.
class FitShowController : public Controller {
    Q_OBJECT
public:
    FitShowController(FitShow::Control &control, DeviceProfile profile,
                      std::function<qint64()> receiveClock, std::function<void()> open,
                      std::function<void()> close, QObject *parent = nullptr);
    Capabilities capabilities() const override;
    void connectDevice(quint64 generation) override;
    void disconnectDevice() override;
    void start(quint64 generation, quint64 id, Target initial) override;
    void setTarget(quint64 generation, quint64 id, Target target) override;
    void stop(quint64 generation, quint64 id) override;
    void pause(quint64 generation, quint64 id) override;
    void advance(qint64 nowMs) override;
    void observeIdentity(const QString &model, const QString &firmware);
signals:
    void frameObserved(QByteArray frame);
    void identityObserved(QString model, QString firmware);
private:
    bool canControl(quint64 generation, quint64 id, Target target, int &speed, int &incline);
    FitShow::Control &control_;
    DeviceProfile profile_;
    std::function<qint64()> receiveClock_;
    std::function<void()> open_, close_;
    quint64 generation_ = 0;
    bool boundsMatch_ = false, identityMatch_ = false, pendingStatus_ = false, hasStatus_ = false;
    FitShow::Status status_;
    qint64 arrivedAtMs_ = -1;
};
}
