#pragma once
#include "workout.h"
#include <QSet>
#include <QMap>

namespace Treadmill {
class Runner : public QObject {
    Q_OBJECT
public:
    enum class State { Disconnected, Preflight, Ready, Starting, Running, Pausing, Paused,
                       Stopping, Completed, Aborted, StopUnconfirmed, Fault };
    // Borrows controller; one session per pair. Destroy Runner before Controller.
    Runner(Controller &controller, Workout workout, QObject *parent = nullptr);
    void prepare(qint64 nowMs);
    void confirmStart();
    void advance(qint64 nowMs);
    void abort(const QString &reason);
    void pause();
    void resume();
    void reduce();
    State state() const { return state_; }
    qint64 elapsedMs() const;
    int stageIndex() const { return stageIndex_; }
    Target requested() const { return requested_; }
    Telemetry telemetry() const { return telemetry_; }
    QString reason() const { return reason_; }
    quint64 generation() const { return generation_; }
    bool finished() const;
    static QString stateName(State state);
signals:
    void logEvent(QString type, QString message);
    void commandSettled(quint64 id);
    void commandTimedOut(quint64 id);
    void stateChanged(QString state);
private:
    void setState(State state);
    void onSample(Telemetry sample);
    void requestTarget(Target target);
    Target quantize(Target target) const;
    bool observed(Target target) const;
    void stop(bool normal, const QString &reason);
    void enterStage();
    Controller &controller_;
    Workout workout_;
    State state_ = State::Disconnected;
    quint64 generation_ = 1;
    quint64 nextId_ = 0;
    quint64 latestId_ = 0;
    qint64 nowMs_ = 0;
    qint64 lastAdvanceMs_ = -1;
    qint64 deadlineMs_ = 0;
    qint64 targetSinceMs_ = 0;
    qint64 stageElapsedMs_ = 0;
    int stageIndex_ = 0;
    int stoppedSamples_ = 0;
    int targetSamples_ = 0;
    bool normalStop_ = false;
    bool stageEnding_ = false;
    double speedCeiling_ = INFINITY;
    double gradeCeiling_ = INFINITY;
    QString reason_;
    Target requested_;
    Telemetry telemetry_;
    QSet<QString> notified_;
    QMap<quint64, qint64> acknowledgementDeadlines_;
};
}
