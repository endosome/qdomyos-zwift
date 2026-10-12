#pragma once
#include "controller.h"
#include <QString>
#include <QVector>

namespace Treadmill {
struct TextEvent { qint64 offsetMs; QString message; };
struct Stage {
    QString name;
    QString description;
    QString instruction;
    qint64 durationMs = 0;
    Target from;
    Target to;
    QVector<TextEvent> events;
    Target targetAt(qint64 elapsedMs) const;
};
struct Workout {
    QString id;
    QString name;
    QVector<Stage> stages;
    qint64 durationMs() const;
};
struct WorkoutLoadResult { Workout workout; QString error; };
WorkoutLoadResult loadWorkout(const QString &path);
QString validateWorkout(const Workout &workout, const Capabilities &capabilities);
}
