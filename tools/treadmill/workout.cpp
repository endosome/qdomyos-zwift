#include "workout.h"
#include <QFile>
#include <QXmlStreamReader>
#include <QSet>
#include <algorithm>

namespace Treadmill {
Target Stage::targetAt(qint64 elapsedMs) const {
    const double progress = durationMs > 0 ? std::clamp(double(elapsedMs) / durationMs, 0.0, 1.0) : 1.0;
    return {from.speedKmh + (to.speedKmh - from.speedKmh) * progress,
            from.gradePercent + (to.gradePercent - from.gradePercent) * progress};
}
qint64 Workout::durationMs() const {
    qint64 duration = 0;
    for (const auto &stage : stages) duration += stage.durationMs;
    return duration;
}
static bool attributesAllowed(const QXmlStreamAttributes &attributes, const QSet<QString> &allowed) {
    for (const auto &attribute : attributes) {
        if (!attribute.namespaceUri().isEmpty() || !allowed.contains(attribute.name().toString())) return false;
    }
    return true;
}
static qint64 durationMs(const QString &value) {
    const auto parts = value.split(':');
    if (parts.size() != 3) return -1;
    QVector<int> numbers;
    for (const auto &part : parts) {
        if (part.size() != 2) return -1;
        for (const auto c : part) if (c < '0' || c > '9') return -1;
        numbers.append(part.toInt());
    }
    if (numbers[0] > 23 || numbers[1] > 59 || numbers[2] > 59) return -1;
    return (numbers[0] * 3600 + numbers[1] * 60 + numbers[2]) * 1000LL;
}
static bool number(const QXmlStreamAttributes &atts, const QString &name, double &value) {
    bool ok = false;
    value = atts.value(name).toDouble(&ok);
    return ok && std::isfinite(value);
}
static bool axis(const QXmlStreamAttributes &atts, const QString &fixed, const QString &from,
                 const QString &to, double &first, double &last) {
    if (atts.hasAttribute(fixed)) {
        if (atts.hasAttribute(from) || atts.hasAttribute(to) || !number(atts, fixed, first)) return false;
        last = first;
        return true;
    }
    return number(atts, from, first) && number(atts, to, last);
}
static bool readStage(QXmlStreamReader &xml, Stage &stage) {
    const auto atts = xml.attributes();
    if (!attributesAllowed(atts, {"name", "description", "instruction", "duration", "forcespeed",
                                  "speed", "speedfrom", "speedto", "inclination", "inclinationfrom", "inclinationto"})) {
        xml.raiseError("Unsupported row attribute");
        return false;
    }
    stage.durationMs = durationMs(atts.value("duration").toString());
    stage.name = atts.value("name").toString().trimmed();
    stage.description = atts.value("description").toString();
    stage.instruction = atts.value("instruction").toString();
    if (stage.name.isEmpty() || stage.durationMs <= 0 || atts.value("forcespeed") != "1" ||
        !axis(atts, "speed", "speedfrom", "speedto", stage.from.speedKmh, stage.to.speedKmh) ||
        !axis(atts, "inclination", "inclinationfrom", "inclinationto", stage.from.gradePercent, stage.to.gradePercent) ||
        stage.from.speedKmh <= 0 || stage.to.speedKmh <= 0 || stage.from.gradePercent < 0 || stage.to.gradePercent < 0) {
        xml.raiseError("Row needs a name, positive hh:mm:ss duration, forcespeed=1 and finite speed/grade targets");
        return false;
    }
    while (xml.readNextStartElement()) {
        if (xml.name() != "textevent" || !attributesAllowed(xml.attributes(), {"timeoffset", "message"})) {
            xml.raiseError("Unsupported row child");
            return false;
        }
        bool ok = false;
        const auto offset = xml.attributes().value("timeoffset").toLongLong(&ok);
        const QString message = xml.attributes().value("message").toString();
        if (!ok || offset < 0 || offset > stage.durationMs / 1000 || message.trimmed().isEmpty()) {
            xml.raiseError("Invalid text event offset/message");
            return false;
        }
        stage.events.append({offset * 1000, message});
        xml.readNext();
        if (!xml.isEndElement()) {
            xml.raiseError("Text events must be empty elements");
            return false;
        }
    }
    return !xml.hasError();
}
WorkoutLoadResult loadWorkout(const QString &path) {
    WorkoutLoadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    if (file.size() > 1024 * 1024) {
        result.error = "Workout file exceeds 1 MiB";
        return result;
    }
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isDTD()) { xml.raiseError("DTD declarations are unsupported"); break; }
        if (xml.isStartElement()) break;
    }
    if (!xml.isStartElement() || xml.name() != "rows") xml.raiseError("Expected rows root");
    const auto atts = xml.attributes();
    if (!attributesAllowed(atts, {"version", "id", "name", "speedunit", "inclineunit", "device"}) ||
        atts.value("version") != "1" || atts.value("speedunit") != "km/h" || atts.value("inclineunit") != "percent" ||
        (atts.hasAttribute("device") && atts.value("device") != "treadmill"))
        xml.raiseError("Require version=1, speedunit=km/h and inclineunit=percent; unsupported root attribute/device");
    result.workout.id = atts.value("id").toString().trimmed();
    result.workout.name = atts.value("name").toString().trimmed();
    if (result.workout.id.isEmpty() || result.workout.name.isEmpty()) xml.raiseError("Workout needs id and name");
    auto append = [&](const Stage &stage) {
        if (result.workout.stages.size() >= 4096 || result.workout.durationMs() + stage.durationMs >= 24 * 3600000LL) {
            xml.raiseError("Workout expansion exceeds 4096 rows or duration reaches 24 hours");
            return;
        }
        result.workout.stages.append(stage);
    };
    while (!xml.hasError() && xml.readNextStartElement()) {
        if (xml.name() == "row") {
            Stage stage;
            if (readStage(xml, stage)) append(stage);
        } else if (xml.name() == "repeat") {
            bool ok = false;
            const int times = xml.attributes().value("times").toInt(&ok);
            if (!attributesAllowed(xml.attributes(), {"times"}) || !ok || times < 1 || times > 100) {
                xml.raiseError("Repeat count must be 1–100");
                break;
            }
            QVector<Stage> block;
            while (xml.readNextStartElement()) {
                if (xml.name() != "row" || block.size() >= 4096) {
                    xml.raiseError("Repeat supports rows only; nested repeats are unsupported");
                    break;
                }
                Stage stage;
                if (!readStage(xml, stage)) break;
                block.append(stage);
            }
            if (block.isEmpty()) xml.raiseError("Empty repeat");
            for (int i = 0; i < times && !xml.hasError(); ++i)
                for (const auto &stage : block) { append(stage); if (xml.hasError()) break; }
        } else {
            xml.raiseError("Unsupported workout element");
        }
    }
    while (!xml.atEnd() && !xml.hasError()) {
        xml.readNext();
        if (xml.isDTD() || xml.isStartElement()) xml.raiseError("Unexpected content after rows");
    }
    if (xml.hasError()) result.error = QString("Line %1: %2").arg(xml.lineNumber()).arg(xml.errorString());
    else if (result.workout.stages.isEmpty()) result.error = "Workout has no stages";
    if (!result.error.isEmpty()) result.workout = {};
    return result;
}
QString validateWorkout(const Workout &workout, const Capabilities &capabilities) {
    if (workout.stages.isEmpty()) return "Workout has no stages";
    for (const auto &stage : workout.stages) {
        if (stage.durationMs <= 0 || !capabilities.accepts(stage.from) || !capabilities.accepts(stage.to))
            return "Stage outside validated capabilities: " + stage.name;
    }
    return {};
}
}
