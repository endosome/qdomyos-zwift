#include "fitworkout.h"
#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <cmath>
#include <sstream>
#include "fit_decode.hpp"
#include "fit_encode.hpp"
#include "fit_file_id_mesg.hpp"
#include "fit_file_id_mesg_listener.hpp"
#include "fit_mesg_broadcaster.hpp"
#include "fit_workout_mesg.hpp"
#include "fit_workout_mesg_listener.hpp"
#include "fit_workout_step_mesg.hpp"
#include "fit_workout_step_mesg_listener.hpp"

namespace Treadmill {
namespace Fit {
namespace {

constexpr FIT_DATE_TIME kCreatedStamp = 1100000000; // fixed so the same spec always yields the same bytes
const QStringList kIntensities = {"active", "rest", "warmup", "cooldown", "recovery", "interval", "other"};

FIT_INTENSITY intensityCode(const QString &name) {
    switch (kIntensities.indexOf(name)) {
    case 0: return FIT_INTENSITY_ACTIVE;
    case 1: return FIT_INTENSITY_REST;
    case 2: return FIT_INTENSITY_WARMUP;
    case 3: return FIT_INTENSITY_COOLDOWN;
    case 4: return FIT_INTENSITY_RECOVERY;
    case 5: return FIT_INTENSITY_INTERVAL;
    default: return FIT_INTENSITY_OTHER;
    }
}
QString intensityName(FIT_INTENSITY code) {
    switch (code) {
    case FIT_INTENSITY_ACTIVE: return "active";
    case FIT_INTENSITY_REST: return "rest";
    case FIT_INTENSITY_WARMUP: return "warmup";
    case FIT_INTENSITY_COOLDOWN: return "cooldown";
    case FIT_INTENSITY_RECOVERY: return "recovery";
    case FIT_INTENSITY_INTERVAL: return "interval";
    default: return "other";
    }
}
double gradeOf(int level) { return level * kGradePerLevel; }
thread_local double gMaxSpeedKmh = kMaxSpeedKmh; // set from the sport before steps are validated

// The FIT SDK keeps a string's length in 8 bits: anything over 255 bytes wraps and its copy overruns the buffer
// (found with AddressSanitizer). So every string we hand it is plain printable ASCII, well under that limit.
bool plainText(const QString &text, const QString &what, QString &error) {
    for (const QChar c : text)
        if (c.unicode() < 0x20 || c.unicode() > 0x7e) { error = what + " must be plain printable ASCII"; return false; }
    return true;
}
bool validateStep(const Step &step, const QString &where, QString &error) {
    if (step.name.isEmpty() || step.name.size() > 32) { error = where + ": name must be 1-32 characters"; return false; }
    if (step.notes.size() > 160) { error = where + ": notes exceed 160 characters"; return false; }
    if (!plainText(step.name, where + ": name", error) || !plainText(step.notes, where + ": notes", error)) return false;
    if (!kIntensities.contains(step.intensity)) { error = where + ": unknown intensity " + step.intensity; return false; }
    if (step.seconds < kMinStepSeconds || step.seconds > kMaxStepSeconds) {
        error = where + QString(": duration must be %1-%2 seconds").arg(kMinStepSeconds).arg(kMaxStepSeconds); return false;
    }
    if (!(step.speedKmh >= kMinSpeedKmh && step.speedKmh <= gMaxSpeedKmh) ||
        std::fabs(step.speedKmh * 10 - std::round(step.speedKmh * 10)) > 1e-6) {
        error = where + QString(": speed %1 must be %2-%3 km/h in 0.1 steps").arg(step.speedKmh).arg(kMinSpeedKmh).arg(gMaxSpeedKmh); return false;
    }
    if (step.level < 0 || step.level > kMaxLevel) { error = where + QString(": level %1 must be 0-%2").arg(step.level).arg(kMaxLevel); return false; }
    return true;
}

// Spec speeds must already be on the 0.1 km/h grid; only ramp interpolation may round.
bool onSpeedGrid(double speed, const QString &where, QString &error) {
    if (std::fabs(speed * 10 - std::round(speed * 10)) > 1e-6) { error = where + QString(": speed %1 must be in 0.1 steps").arg(speed); return false; }
    return true;
}
bool allowedKeys(const QJsonObject &object, const QSet<QString> &allowed, const QString &where, QString &error) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key())) { error = where + ": unknown key '" + it.key() + "'"; return false; }
    return true;
}
bool numberField(const QJsonObject &object, const QString &key, double &value, const QString &where, QString &error) {
    const QJsonValue v = object.value(key);
    if (!v.isDouble()) { error = where + ": '" + key + "' must be a number"; return false; }
    value = v.toDouble();
    return std::isfinite(value);
}
bool secondsField(const QJsonObject &object, const QString &minutesKey, const QString &secondsKey, int &seconds, const QString &where, QString &error) {
    const bool hasMinutes = object.contains(minutesKey), hasSeconds = object.contains(secondsKey);
    if (hasMinutes == hasSeconds) { error = where + ": give exactly one of '" + minutesKey + "' or '" + secondsKey + "'"; return false; }
    double value = 0;
    if (!numberField(object, hasMinutes ? minutesKey : secondsKey, value, where, error)) return false;
    const double total = hasMinutes ? value * 60 : value;
    if (std::fabs(total - std::round(total)) > 1e-6) { error = where + ": duration must be a whole number of seconds"; return false; }
    seconds = int(std::round(total));
    return true;
}
Step makeStep(const QString &name, int seconds, double speed, int level, const QString &intensity, const QString &userNotes) {
    Step step;
    step.name = name; step.seconds = seconds; step.speedKmh = std::round(speed * 10) / 10; step.level = level; step.intensity = intensity;
    step.notes = stepSummary(step.speedKmh, level) + (userNotes.isEmpty() ? QString() : " " + userNotes);
    return step;
}
bool parseOneStep(const QJsonObject &object, Step &step, const QString &where, QString &error) {
    if (!allowedKeys(object, {"name", "minutes", "seconds", "speed", "level", "intensity", "notes"}, where, error)) return false;
    int seconds = 0; double speed = 0, level = 0;
    if (!secondsField(object, "minutes", "seconds", seconds, where, error) || !numberField(object, "speed", speed, where, error) ||
        !numberField(object, "level", level, where, error)) return false;
    if (std::fabs(level - std::round(level)) > 1e-9) { error = where + ": level must be a whole number"; return false; }
    if (!onSpeedGrid(speed, where, error)) return false;
    step = makeStep(object.value("name").toString(), seconds, speed, int(std::round(level)), object.value("intensity").toString("active"),
                    object.value("notes").toString());
    return validateStep(step, where, error);
}
bool parseRamp(const QJsonObject &object, QVector<Step> &out, const QString &where, QString &error) {
    if (!allowedKeys(object, {"name", "minutes", "seconds", "speed_from", "speed_to", "level_from", "level_to", "step_minutes", "step_seconds", "intensity", "notes"}, where, error)) return false;
    int total = 0, each = 60; double speedFrom = 0, speedTo = 0, levelFrom = 0, levelTo = 0;
    if (!secondsField(object, "minutes", "seconds", total, where, error) || !numberField(object, "speed_from", speedFrom, where, error) ||
        !numberField(object, "speed_to", speedTo, where, error) || !numberField(object, "level_from", levelFrom, where, error) ||
        !numberField(object, "level_to", levelTo, where, error)) return false;
    if (!onSpeedGrid(speedFrom, where, error) || !onSpeedGrid(speedTo, where, error)) return false;
    if (std::fabs(levelFrom - std::round(levelFrom)) > 1e-9 || std::fabs(levelTo - std::round(levelTo)) > 1e-9) {
        error = where + ": levels must be whole numbers"; return false;
    }
    if ((object.contains("step_minutes") || object.contains("step_seconds")) &&
        !secondsField(object, "step_minutes", "step_seconds", each, where, error)) return false;
    if (each < kMinStepSeconds || total % each != 0 || total / each < 2) {
        error = where + ": ramp needs at least two equal steps of 30 s or more that fill its duration"; return false;
    }
    const int count = total / each;
    const QString base = object.value("name").toString();
    for (int k = 0; k < count; ++k) {
        const double t = double(k) / (count - 1);
        Step step = makeStep(QString("%1 %2/%3").arg(base).arg(k + 1).arg(count), each, speedFrom + (speedTo - speedFrom) * t,
                             int(std::lround(levelFrom + (levelTo - levelFrom) * t)), object.value("intensity").toString("active"),
                             object.value("notes").toString());
        if (!validateStep(step, where, error)) return false;
        out.append(step);
    }
    return true;
}
bool parseItems(const QJsonArray &items, QVector<Node> &nodes, bool insideRepeat, const QString &where, QString &error) {
    for (int i = 0; i < items.size(); ++i) {
        const QString here = QString("%1[%2]").arg(where).arg(i + 1);
        if (!items[i].isObject()) { error = here + ": expected an object"; return false; }
        const QJsonObject object = items[i].toObject();
        if (object.contains("repeat")) {
            if (insideRepeat) { error = here + ": repeats cannot be nested"; return false; }
            if (!allowedKeys(object, {"repeat", "steps"}, here, error)) return false;
            const double times = object.value("repeat").toDouble(-1);
            if (times < 2 || times > 40 || times != std::floor(times)) { error = here + ": repeat must be a whole number from 2 to 40"; return false; }
            QVector<Node> inner;
            if (!object.value("steps").isArray() || !parseItems(object.value("steps").toArray(), inner, true, here + ".steps", error)) {
                if (error.isEmpty()) error = here + ": 'steps' must be an array";
                return false;
            }
            if (inner.isEmpty()) { error = here + ": empty repeat"; return false; }
            Node node; node.isRepeat = true; node.times = int(times);
            for (const Node &n : inner) node.body.append(n.step);
            nodes.append(node);
        } else if (object.contains("ramp")) {
            if (object.size() != 1 || !object.value("ramp").isObject()) { error = here + ": 'ramp' must be the only key and an object"; return false; }
            QVector<Step> steps;
            if (!parseRamp(object.value("ramp").toObject(), steps, here + ".ramp", error)) return false;
            for (const Step &s : steps) { Node n; n.step = s; nodes.append(n); }
        } else {
            Node node;
            if (!parseOneStep(object, node.step, here, error)) return false;
            nodes.append(node);
        }
    }
    return true;
}
QJsonObject stepJson(const Step &step) {
    return {{"name", step.name}, {"seconds", step.seconds}, {"speed", step.speedKmh}, {"level", step.level},
            {"intensity", step.intensity}, {"notes", step.notes}};
}

fit::WorkoutStepMesg stepMesg(const Step &step, int index) {
    fit::WorkoutStepMesg mesg;
    mesg.SetMessageIndex(FIT_MESSAGE_INDEX(index));
    mesg.SetWktStepName(step.name.toStdWString());
    mesg.SetDurationType(FIT_WKT_STEP_DURATION_TIME);
    mesg.SetDurationTime(FIT_FLOAT32(step.seconds));
    mesg.SetTargetType(FIT_WKT_STEP_TARGET_SPEED);
    mesg.SetTargetValue(0); // 0 = custom range
    // A +/- 0.2 km/h band: with identical low and high a watch would alert constantly.
    mesg.SetCustomTargetSpeedLow(FIT_FLOAT32(std::round((step.speedKmh - kSpeedBandKmh) / 3.6 * 1000.0) / 1000.0));
    mesg.SetCustomTargetSpeedHigh(FIT_FLOAT32(std::round((step.speedKmh + kSpeedBandKmh) / 3.6 * 1000.0) / 1000.0));
    mesg.SetIntensity(intensityCode(step.intensity));
    mesg.SetNotes(step.notes.toStdWString());
    mesg.SetSecondaryTargetType(FIT_WKT_STEP_TARGET_GRADE);
    mesg.SetSecondaryTargetValue(0);
    const FIT_UINT32 hundredths = FIT_UINT32(std::lround(gradeOf(step.level) * 100.0));
    mesg.SetSecondaryCustomTargetValueLow(hundredths);
    mesg.SetSecondaryCustomTargetValueHigh(hundredths);
    return mesg;
}

struct RawStep {
    int index = 0;
    QString name, notes;
    FIT_WKT_STEP_DURATION durationType = FIT_WKT_STEP_DURATION_INVALID;
    double seconds = 0, speedLow = 0, speedHigh = 0;
    FIT_WKT_STEP_TARGET target = FIT_WKT_STEP_TARGET_INVALID, secondary = FIT_WKT_STEP_TARGET_INVALID;
    quint32 gradeLow = 0, gradeHigh = 0, firstStep = 0, repeats = 0;
    FIT_INTENSITY intensity = FIT_INTENSITY_INVALID;
};
class Collector : public fit::FileIdMesgListener, public fit::WorkoutMesgListener, public fit::WorkoutStepMesgListener {
public:
    bool sawFileId = false;
    FIT_FILE fileType = FIT_FILE_INVALID;
    QString name, description;
    FIT_SPORT sport = FIT_SPORT_INVALID;
    FIT_SUB_SPORT subSport = FIT_SUB_SPORT_INVALID;
    QVector<RawStep> steps;
    void OnMesg(fit::FileIdMesg &mesg) override { sawFileId = true; fileType = mesg.GetType(); }
    void OnMesg(fit::WorkoutMesg &mesg) override {
        name = QString::fromStdWString(mesg.GetWktName());
        description = QString::fromStdWString(mesg.GetWktDescription());
        sport = mesg.GetSport();
        subSport = mesg.GetSubSport();
    }
    void OnMesg(fit::WorkoutStepMesg &mesg) override {
        RawStep raw;
        raw.index = int(mesg.GetMessageIndex());
        raw.name = QString::fromStdWString(mesg.GetWktStepName());
        raw.notes = QString::fromStdWString(mesg.GetNotes());
        raw.durationType = mesg.GetDurationType();
        raw.intensity = mesg.GetIntensity();
        raw.target = mesg.GetTargetType();
        raw.secondary = mesg.GetSecondaryTargetType();
        if (raw.durationType == FIT_WKT_STEP_DURATION_TIME) raw.seconds = mesg.GetDurationTime();
        if (raw.durationType == FIT_WKT_STEP_DURATION_REPEAT_UNTIL_STEPS_CMPLT) {
            raw.firstStep = mesg.GetDurationStep();
            raw.repeats = mesg.GetRepeatSteps();
        }
        if (raw.target == FIT_WKT_STEP_TARGET_SPEED) { raw.speedLow = mesg.GetCustomTargetSpeedLow(); raw.speedHigh = mesg.GetCustomTargetSpeedHigh(); }
        if (raw.secondary == FIT_WKT_STEP_TARGET_GRADE) { raw.gradeLow = mesg.GetSecondaryCustomTargetValueLow(); raw.gradeHigh = mesg.GetSecondaryCustomTargetValueHigh(); }
        steps.append(raw);
    }
};

} // namespace

// Plain ASCII, incline first: watches that show notes show the start, and one forum report has notes cut at "~".
QString stepSummary(double speedKmh, int level) {
    const double grade = gradeOf(level);
    if (level == 0) return QString("Flat (L0), %1 km/h.").arg(speedKmh, 0, 'f', 1);
    return QString("Incline %1% (L%2), %3 km/h, about %4 m/h up.").arg(grade, 0, 'f', 1).arg(level).arg(speedKmh, 0, 'f', 1)
        .arg(int(std::lround(speedKmh * grade * 10)));
}

bool fromSpec(const QJsonObject &spec, Training &out, QString &error) {
    out = Training();
    if (!allowedKeys(spec, {"id", "name", "description", "sport", "sub_sport", "steps"}, "spec", error)) return false;
    out.name = spec.value("name").toString();
    out.description = spec.value("description").toString();
    out.sport = spec.value("sport").toString("hiking");
    if (out.name.isEmpty() || out.name.size() > 40) { error = "spec: name must be 1-40 characters"; return false; }
    if (out.description.size() > 250) { error = "spec: description exceeds 250 characters"; return false; }
    if (!plainText(out.name, "spec: name", error) || !plainText(out.description, "spec: description", error)) return false;
    if (out.sport != "hiking" && out.sport != "walking" && out.sport != "running") { error = "spec: sport must be hiking, walking or running"; return false; }
    out.subSport = spec.value("sub_sport").toString(out.sport == "running" ? "treadmill" : "");
    if (out.sport != "running" && !out.subSport.isEmpty()) { error = "spec: sub_sport applies to running only"; return false; }
    if (out.sport == "running" && out.subSport != "treadmill" && out.subSport != "trail") { error = "spec: sub_sport must be treadmill or trail"; return false; }
    gMaxSpeedKmh = out.sport == "running" ? kMaxRunningSpeedKmh : kMaxSpeedKmh;
    if (!spec.value("steps").isArray() || spec.value("steps").toArray().isEmpty()) { error = "spec: 'steps' must be a non-empty array"; return false; }
    if (!parseItems(spec.value("steps").toArray(), out.nodes, false, "steps", error)) return false;
    if (totalSeconds(out) > kMaxTrainingSeconds) {
        error = QString("training is %1 min; the W1 limit is %2").arg(totalSeconds(out) / 60.0, 0, 'f', 1).arg(kMaxTrainingSeconds / 60);
        return false;
    }
    if (flatten(out).size() > 400) { error = "training has more than 400 steps after expanding repeats"; return false; }
    int messages = 0;
    for (const Node &node : out.nodes) messages += node.isRepeat ? int(node.body.size()) + 1 : 1;
    if (messages > kGarminStepLimit) {
        error = QString("training needs %1 workout steps; Garmin Connect is reported to accept %2. Use longer ramp steps or fewer steps")
                    .arg(messages).arg(kGarminStepLimit);
        return false;
    }
    return true;
}

QVector<Step> flatten(const Training &training) {
    QVector<Step> steps;
    for (const Node &node : training.nodes) {
        if (!node.isRepeat) { steps.append(node.step); continue; }
        for (int i = 0; i < node.times; ++i) steps += node.body;
    }
    return steps;
}
int totalSeconds(const Training &training) {
    int total = 0;
    for (const Step &step : flatten(training)) total += step.seconds;
    return total;
}
double verticalMetersEstimate(const Training &training) {
    double meters = 0;
    for (const Step &step : flatten(training)) meters += step.speedKmh * gradeOf(step.level) * 10.0 * step.seconds / 3600.0;
    return meters;
}
QJsonObject toJson(const Training &training) {
    QJsonArray nodes;
    for (const Node &node : training.nodes) {
        if (!node.isRepeat) { nodes.append(stepJson(node.step)); continue; }
        QJsonArray body;
        for (const Step &step : node.body) body.append(stepJson(step));
        nodes.append(QJsonObject{{"repeat", node.times}, {"steps", body}});
    }
    QJsonObject json{{"name", training.name}, {"description", training.description}, {"sport", training.sport}, {"steps", nodes}};
    if (!training.subSport.isEmpty()) json.insert("sub_sport", training.subSport);
    return json;
}

QByteArray encode(const Training &training, QString &error) {
    std::vector<fit::WorkoutStepMesg> messages;
    for (const Node &node : training.nodes) {
        if (!node.isRepeat) { messages.push_back(stepMesg(node.step, int(messages.size()))); continue; }
        const int first = int(messages.size());
        for (const Step &step : node.body) messages.push_back(stepMesg(step, int(messages.size())));
        fit::WorkoutStepMesg repeat;
        repeat.SetMessageIndex(FIT_MESSAGE_INDEX(messages.size()));
        repeat.SetDurationType(FIT_WKT_STEP_DURATION_REPEAT_UNTIL_STEPS_CMPLT);
        repeat.SetDurationStep(FIT_UINT32(first));
        repeat.SetTargetType(FIT_WKT_STEP_TARGET_OPEN);
        repeat.SetRepeatSteps(FIT_UINT32(node.times));
        messages.push_back(repeat);
    }
    fit::FileIdMesg fileId;
    fileId.SetType(FIT_FILE_WORKOUT);
    fileId.SetManufacturer(FIT_MANUFACTURER_DEVELOPMENT);
    fileId.SetProduct(1);
    fileId.SetSerialNumber(1);
    fileId.SetTimeCreated(kCreatedStamp);
    fit::WorkoutMesg workout;
    workout.SetSport(training.sport == "walking" ? FIT_SPORT_WALKING : training.sport == "running" ? FIT_SPORT_RUNNING : FIT_SPORT_HIKING);
    if (training.sport == "running") workout.SetSubSport(training.subSport == "trail" ? FIT_SUB_SPORT_TRAIL : FIT_SUB_SPORT_TREADMILL);
    workout.SetNumValidSteps(FIT_UINT16(messages.size()));
    workout.SetWktName(training.name.toStdWString());
    if (!training.description.isEmpty()) workout.SetWktDescription(training.description.toStdWString());
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    fit::Encode encoder(fit::ProtocolVersion::V20);
    encoder.Open(stream);
    encoder.Write(fileId);
    encoder.Write(workout);
    for (const auto &message : messages) encoder.Write(message);
    if (!encoder.Close()) { error = "FIT encoder failed to finish the file"; return {}; }
    const std::string bytes = stream.str();
    return QByteArray(bytes.data(), int(bytes.size()));
}

bool decode(const QByteArray &bytes, Training &out, QString &error) {
    out = Training();
    std::stringstream stream(std::string(bytes.constData(), size_t(bytes.size())), std::ios::in | std::ios::binary);
    fit::Decode decoder;
    if (!decoder.IsFIT(stream)) { error = "not a FIT file"; return false; }
    if (!decoder.CheckIntegrity(stream)) { error = "FIT integrity check failed (header or CRC)"; return false; }
    stream.clear(); stream.seekg(0);
    fit::MesgBroadcaster broadcaster;
    Collector collector;
    broadcaster.AddListener(static_cast<fit::FileIdMesgListener &>(collector));
    broadcaster.AddListener(static_cast<fit::WorkoutMesgListener &>(collector));
    broadcaster.AddListener(static_cast<fit::WorkoutStepMesgListener &>(collector));
    try {
        if (!decoder.Read(stream, broadcaster, broadcaster)) { error = "FIT decoding failed"; return false; }
    } catch (const fit::RuntimeException &exception) {
        error = QString("FIT decoding failed: ") + exception.what(); return false;
    }
    if (!collector.sawFileId || collector.fileType != FIT_FILE_WORKOUT) { error = "FIT file is not a workout"; return false; }
    if (collector.sport != FIT_SPORT_HIKING && collector.sport != FIT_SPORT_WALKING && collector.sport != FIT_SPORT_RUNNING) { error = "workout sport is not hiking, walking or running"; return false; }
    out.name = collector.name;
    out.description = collector.description;
    out.sport = collector.sport == FIT_SPORT_WALKING ? "walking" : collector.sport == FIT_SPORT_RUNNING ? "running" : "hiking";
    if (out.sport == "running") out.subSport = collector.subSport == FIT_SUB_SPORT_TRAIL ? "trail" : "treadmill";
    gMaxSpeedKmh = out.sport == "running" ? kMaxRunningSpeedKmh : kMaxSpeedKmh;
    struct Pending { Node node; int firstIndex; };
    QVector<Pending> pending;
    for (int i = 0; i < collector.steps.size(); ++i) {
        const RawStep &raw = collector.steps[i];
        const QString where = QString("step %1").arg(i + 1);
        if (raw.index != i) { error = where + ": steps are not in message_index order"; return false; }
        if (raw.durationType == FIT_WKT_STEP_DURATION_REPEAT_UNTIL_STEPS_CMPLT) {
            int at = -1;
            for (int p = 0; p < pending.size(); ++p) if (pending[p].firstIndex == int(raw.firstStep)) { at = p; break; }
            if (at < 0 || raw.repeats < 2 || raw.repeats > 40) { error = where + ": invalid repeat"; return false; }
            Node repeat; repeat.isRepeat = true; repeat.times = int(raw.repeats);
            for (int p = at; p < pending.size(); ++p) {
                if (pending[p].node.isRepeat) { error = where + ": nested repeats are not supported"; return false; }
                repeat.body.append(pending[p].node.step);
            }
            pending.resize(at);
            pending.append({repeat, int(raw.firstStep)});
            continue;
        }
        if (raw.durationType != FIT_WKT_STEP_DURATION_TIME || raw.target != FIT_WKT_STEP_TARGET_SPEED || raw.secondary != FIT_WKT_STEP_TARGET_GRADE) {
            error = where + ": not a W1 step (needs a time duration, a speed target and a grade secondary target)"; return false;
        }
        if (std::fabs((raw.speedHigh - raw.speedLow) - 2 * kSpeedBandKmh / 3.6) > 0.004) { error = where + ": speed range is not the +/- 0.2 km/h band"; return false; }
        const double grade = raw.gradeLow / 100.0;
        const int level = int(std::lround(grade / kGradePerLevel));
        if (raw.gradeLow != raw.gradeHigh || std::fabs(grade - level * kGradePerLevel) > 0.02) { error = where + ": grade is not on a W1 level"; return false; }
        Step step;
        step.name = raw.name; step.notes = raw.notes; step.intensity = intensityName(raw.intensity);
        step.seconds = int(std::lround(raw.seconds));
        step.speedKmh = std::round((raw.speedLow + raw.speedHigh) / 2 * 3.6 * 10) / 10;
        step.level = level;
        if (!validateStep(step, where, error)) return false;
        Node node; node.step = step;
        pending.append({node, i});
    }
    for (const Pending &p : pending) out.nodes.append(p.node);
    if (out.nodes.isEmpty()) { error = "workout has no steps"; return false; }
    if (totalSeconds(out) > kMaxTrainingSeconds) { error = "workout exceeds the 120 minute limit"; return false; }
    if (flatten(out).size() > 400) { error = "workout has more than 400 steps after expanding repeats"; return false; }
    return true;
}

Workout toWorkout(const Training &training, const QString &id) {
    Workout workout;
    workout.id = id;
    workout.name = training.name;
    for (const Step &step : flatten(training)) {
        Stage stage;
        stage.name = step.name;
        stage.description = step.notes;
        stage.instruction = step.notes;
        stage.durationMs = qint64(step.seconds) * 1000;
        stage.from = stage.to = Target{step.speedKmh, gradeOf(step.level)};
        workout.stages.append(stage);
    }
    return workout;
}

} // namespace Fit
} // namespace Treadmill
