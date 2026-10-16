#include "workoutfile.h"
#include "fit/fitworkout.h"
#include <QFile>
#include <QFileInfo>

namespace Treadmill {
WorkoutLoadResult loadWorkoutFile(const QString &path) {
    if (!path.endsWith(".fit", Qt::CaseInsensitive)) return loadWorkout(path);
    WorkoutLoadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    if (file.size() > 1024 * 1024) { result.error = "Workout file exceeds 1 MiB"; return result; }
    Fit::Training training;
    if (!Fit::decode(file.readAll(), training, result.error)) return result;
    const QString id = QFileInfo(path).completeBaseName();
    for (const QChar c : id)
        if (!c.isLetterOrNumber() && c != '-' && c != '_' && c != '.') { result.error = "FIT workout file names may use letters, digits, '-', '_' and '.' only"; return result; }
    result.workout = Fit::toWorkout(training, id);
    return result;
}
}
