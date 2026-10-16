#pragma once
// Trainings stored as Garmin FIT workout files, written and read with the official FIT SDK
// (src/fit-sdk). A training is a list of steps and one level of repeats. Each step has a
// duration, a speed (primary target, a standard FIT custom speed range) and a W1 incline
// level 0-8 (secondary target GRADE).
//
// Grade convention: FIT defines no scale for the grade target (its custom ranges have typed
// fields for speed, heart rate, cadence and power only), and Garmin's workout builder has no
// incline target, so we store grade as hundredths of a percent, the scale Garmin uses for
// record.grade, using the working W1 mapping of 1.375 % per level. Watches ignore that
// secondary target (our CLI reads it). What watches do show is the step notes, which start
// with the incline in plain ASCII, and the speed target, a +/- 0.2 km/h band so a watch's own
// alert stays usable. The completed run carries the incline as a FIT activity (see activity.h).
#include "../workout.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace Treadmill {
namespace Fit {

constexpr double kGradePerLevel = 11.0 / 8.0;
constexpr int kMaxLevel = 8;
constexpr double kMinSpeedKmh = 1.0, kMaxSpeedKmh = 6.0;        // walking mode
constexpr double kMaxRunningSpeedKmh = 12.0;                    // running mode (sport "running")
constexpr double kSpeedBandKmh = 0.2;                           // the watch-facing target is speed +/- this
constexpr int kMinStepSeconds = 30, kMaxStepSeconds = 3600, kMaxTrainingSeconds = 120 * 60;
constexpr int kGarminStepLimit = 50;                            // workout_step messages Garmin Connect is reported to accept

struct Step {
    QString name;
    QString notes;     // final text, including the generated "L4 ~5.5 %" summary
    QString intensity; // active, rest, warmup, cooldown, recovery, interval or other
    int seconds = 0;
    double speedKmh = 0;
    int level = 0;
};
struct Node {
    bool isRepeat = false;
    int times = 1;        // repeat count
    QVector<Step> body;   // steps of a repeat block
    Step step;            // the step when not a repeat
};
struct Training {
    QString name;
    QString description;
    QString sport = QStringLiteral("hiking"); // hiking, walking or running (running allows speeds above 6 km/h)
    QString subSport;                         // running only: "treadmill" (default) or "trail"
    QVector<Node> nodes;
};

// Builds a Training from a JSON spec (see trainings/README.md), expanding ramps into steps and
// enforcing the W1's limits. Returns false and sets error on any problem.
bool fromSpec(const QJsonObject &spec, Training &out, QString &error);
QJsonObject toJson(const Training &training);
QVector<Step> flatten(const Training &training);
int totalSeconds(const Training &training);
double verticalMetersEstimate(const Training &training); // speed x grade x time with the working grade map
QString stepSummary(double speedKmh, int level);

QByteArray encode(const Training &training, QString &error);
bool decode(const QByteArray &bytes, Training &out, QString &error);
Workout toWorkout(const Training &training, const QString &id);

} // namespace Fit
} // namespace Treadmill
