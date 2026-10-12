#ifndef FITSHOWCONTROL_H
#define FITSHOWCONTROL_H
#include "fitshowpacket.h"
#include <QObject>
#include <QList>

namespace FitShow {
struct Limits {
    int minSpeed = -1, maxSpeed = -1, unit = -1;
    int minIncline = -1, maxIncline = -1;
    bool pause = false;
    bool complete() const {
        return minSpeed >= 0 && maxSpeed > 0 && maxSpeed <= 255 && maxSpeed >= minSpeed &&
               minIncline >= 0 && maxIncline >= minIncline && maxIncline <= 127;
    }
    bool accepts(int speed, int incline) const {
        return complete() && speed > 0 && speed >= minSpeed && speed <= maxSpeed &&
               incline >= minIncline && incline <= maxIncline;
    }
};
struct Status {
    int state = -1;
    int speedTenths = -1;
    int inclineCode = -1;
    bool hasMotorData = false;
};
struct Response {
    bool valid = false;
    int command = -1, parameter = -1;
    Limits limits;
    Status status;
};
Response decodeResponse(const QByteArray &frame);

// Opt-in asynchronous queue for standalone control. QZ's legacy FIFO is unchanged.
class Control : public QObject {
    Q_OBJECT
public:
    explicit Control(QObject *parent = nullptr) : QObject(parent) {}
    void setTransport(bool available);
    void allowDiagnostics(bool allowed);
    void queryLimits(quint64 id);
    bool arm(quint64 generation);
    void start(quint64 generation, quint64 id, int speed, int incline);
    void target(quint64 generation, quint64 id, int speed, int incline);
    void stop(quint64 generation, quint64 id);
    void pause(quint64 generation, quint64 id);
    void pump(qint64 nowMs);
    void receive(const QByteArray &frame);
    Limits limits() const { return limits_; }
    quint64 generation() const { return generation_; }
    bool transportAvailable() const { return available_; }
    void cancel();
signals:
    void payloadReady(QByteArray payload, quint64 generation, quint64 id);
    void commandEvent(quint64 id, QString phase);
    void limitsChanged(FitShow::Limits limits);
    void statusChanged(FitShow::Status status);
    void connectionChanged(bool connected);
    void fault(QString reason);
private:
    enum class Kind { Query, Start, InitialTarget, Target, Stop, Pause };
    struct Pending { Kind kind; QByteArray payload; quint64 id; quint64 generation; };
    void enqueue(Kind kind, QByteArray payload, quint64 id);
    bool motorRequest(quint64 generation, quint64 id, int speed, int incline);
    void clearPending();
    QList<Pending> pending_;
    Pending inFlight_ {Kind::Query, {}, 0, 0};
    bool sent_ = false;
    bool available_ = false;
    bool diagnostics_ = false;
    bool armed_ = false;
    bool everArmed_ = false;
    quint64 generation_ = 0;
    qint64 nowMs_ = 0;
    qint64 deadlineMs_ = 0;
    qint64 lastPollMs_ = -500;
    Limits limits_;
};
}
Q_DECLARE_METATYPE(FitShow::Limits)
Q_DECLARE_METATYPE(FitShow::Status)
#endif
