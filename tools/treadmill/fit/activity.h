#pragma once
// Completed runs as Garmin FIT activity files, so Garmin Connect (and watches that sync to it) see the
// distance, speed and elevation gain of a treadmill session. A planned FIT workout cannot carry incline
// to a watch; a recorded activity can. The input is the CLI's JSON Lines run log: the treadmill's own
// reported speed and incline level, sampled at 1 Hz. Altitude is an ESTIMATE: speed x grade, with the
// working W1 grade table (1.375 % per level), because the W1 reports levels, not measured angles.
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace Treadmill {
namespace Fit {

struct ActivitySummary {
    double seconds = 0, distanceMeters = 0, ascentMeters = 0, maxSpeedKmh = 0;
    int records = 0;
};

// sport: hiking, walking or running. Returns an empty array and sets error on failure.
QByteArray buildActivity(const QVector<QJsonObject> &logRows, const QString &sport, ActivitySummary &summary, QString &error);

} // namespace Fit
} // namespace Treadmill
