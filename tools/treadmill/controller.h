#pragma once
#include <QObject>
#include <QVector>
#include <cmath>

namespace Treadmill {
struct Target {
    double speedKmh = 0;
    double gradePercent = 0;
};
struct Capabilities {
    bool validated = false;
    bool pauseSupported = false;
    double minSpeedKmh = 0;
    double maxSpeedKmh = 0;
    double speedStepKmh = 0;
    double minGradePercent = 0;
    double maxGradePercent = 0;
    double gradeStepPercent = 0;
    QVector<double> gradeLevels;
    double maxGradeMappingLoss = INFINITY;
    bool accepts(Target target) const;
    bool mapTarget(Target requested, Target &mapped) const;
};
struct Telemetry {
    qint64 atMs = -1;
    bool connected = false;
    bool running = false;
    bool paused = false;
    Target actual;
};
// Requests acknowledge submission only. Outcomes and physical state arrive as events.
class Controller : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual Capabilities capabilities() const = 0;
    virtual void connectDevice(quint64 generation) = 0;
    virtual void disconnectDevice() = 0;
    virtual void start(quint64 generation, quint64 id, Target initial) = 0;
    virtual void setTarget(quint64 generation, quint64 id, Target target) = 0;
    virtual void stop(quint64 generation, quint64 id) = 0;
    virtual void pause(quint64 generation, quint64 id) = 0;
    virtual void advance(qint64 nowMs) = 0;
signals:
    void sample(Treadmill::Telemetry telemetry);
    void connectionChanged(bool connected);
    void commandEvent(quint64 id, QString phase);
    void fault(QString reason);
};

class Simulator : public Controller {
    Q_OBJECT
public:
    enum class Failure { None, NoAcknowledgements, StaleTelemetry, Disconnect, ManualOverride, StopFailure, RejectTarget };
    explicit Simulator(QObject *parent = nullptr);
    Capabilities capabilities() const override;
    void connectDevice(quint64 generation) override;
    void disconnectDevice() override;
    void start(quint64 generation, quint64 id, Target initial) override;
    void setTarget(quint64 generation, quint64 id, Target target) override;
    void stop(quint64 generation, quint64 id) override;
    void pause(quint64 generation, quint64 id) override;
    void advance(qint64 nowMs) override;
    void inject(Failure failure) { failure_ = failure; }
private:
    bool accept(quint64 generation, quint64 id, Target target);
    void submit(quint64 id);
    struct Ack { quint64 id; qint64 dueMs; };
    QVector<Ack> acknowledgements_;
    quint64 generation_ = 0;
    qint64 nowMs_ = 0;
    qint64 lastSampleMs_ = -1000;
    qint64 startAtMs_ = -1;
    bool hasConnected_ = false;
    bool stopping_ = false;
    bool pausePending_ = false;
    Target target_;
    Telemetry telemetry_;
    Failure failure_ = Failure::None;
};
}
