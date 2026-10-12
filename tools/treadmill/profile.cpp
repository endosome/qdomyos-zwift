#include "profile.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>

namespace Treadmill {
bool DeviceProfile::motorEvidenceComplete() const {
    return diagnosticsConfirmed && controlsVerified && speedFeedbackVerified && inclineFeedbackVerified && stopFeedbackVerified &&
           !model.trimmed().isEmpty() && model.trimmed().compare("unknown", Qt::CaseInsensitive) != 0 && !firmware.trimmed().isEmpty() && firmware.trimmed().compare("unknown", Qt::CaseInsensitive) != 0 && !evidence.trimmed().isEmpty();
}
QString DeviceProfile::validate() const {
    const auto parts = address.split(':');
    if (parts.size() != 6 || address == "00:00:00:00:00:00") return "Invalid Bluetooth identity address";
    for (const auto &part : parts) {
        bool ok = false;
        part.toUInt(&ok, 16);
        if (!ok || part.size() != 2) return "Invalid Bluetooth identity address";
        for (const auto c : part.toUpper()) if (!(c >= '0' && c <= '9') && !(c >= 'A' && c <= 'F')) return "Invalid Bluetooth identity address";
    }
    if (name.trimmed().isEmpty()) return "Device name is required";
    if (!((service == 0xffe0 && write == 0xffe1 && notify == 0xffe4) ||
          (service == 0xfff0 && write == 0xfff2 && notify == 0xfff1) ||
          (service == 0xae00 && write == 0xae01 && notify == 0xae02))) return "Unsupported/mismatched FitShow GATT mapping";
    if (speedUnit != SpeedUnit::Kmh && speedUnit != SpeedUnit::Mph) return "Speed units are unverified/unknown";
    if (minSpeedCode < 0 || maxSpeedCode > 255 || maxSpeedCode < minSpeedCode ||
        runningMinCode <= 0 || runningMinCode < minSpeedCode || runningMinCode > maxSpeedCode || speedStepCode < 1 || speedStepCode > 255 ||
        unitTag < -1 || unitTag > 255) return "Invalid speed byte limits/resolution/unit tag";
    if (inclines.isEmpty() || !std::isfinite(maxGradeMappingLoss) || maxGradeMappingLoss < 0) return "Incline mapping/tolerance is required";
    int lastCode = -1;
    double lastGrade = -1;
    for (const auto &point : inclines) {
        if (point.code <= lastCode || point.code > 127 || !std::isfinite(point.percent) || point.percent < 0 || point.percent <= lastGrade)
            return "Incline codes/grades must be finite, nonnegative and strictly increasing";
        lastCode = point.code; lastGrade = point.percent;
    }
    if (controlsVerified && !motorEvidenceComplete()) return "Motor verification requires identity, evidence, diagnostics approval and all feedback checks";
    return {};
}
Capabilities DeviceProfile::capabilities() const {
    Capabilities result;
    if (!validate().isEmpty()) return result;
    result.validated = motorEvidenceComplete();
    result.pauseSupported = result.validated && pauseVerified;
    result.minSpeedKmh = runningMinCode * speedScale() / 10;
    result.maxSpeedKmh = maxSpeedCode * speedScale() / 10;
    result.speedStepKmh = speedStepCode * speedScale() / 10;
    result.minGradePercent = inclines.first().percent;
    result.maxGradePercent = inclines.last().percent;
    result.gradeStepPercent = 0;
    result.maxGradeMappingLoss = maxGradeMappingLoss;
    for (const auto &point : inclines) result.gradeLevels.append(point.percent);
    return result;
}
bool DeviceProfile::matches(const FitShow::Limits &limits) const {
    return validate().isEmpty() && limits.complete() && minSpeedCode == limits.minSpeed && maxSpeedCode == limits.maxSpeed &&
           inclines.first().code == limits.minIncline && inclines.last().code == limits.maxIncline &&
           (unitTag < 0 || unitTag == limits.unit);
}
bool DeviceProfile::map(Target requested, Target &mapped) const {
    if (!validate().isEmpty() || !capabilities().mapTarget(requested, mapped)) return false;
    return requested.gradePercent - mapped.gradePercent <= maxGradeMappingLoss + 1e-8;
}
bool DeviceProfile::toWire(Target target, int &speed, int &incline) const {
    if (!validate().isEmpty()) return false;
    Target mapped;
    if (!map(target, mapped) || std::abs(mapped.speedKmh - target.speedKmh) > 1e-7 ||
        std::abs(mapped.gradePercent - target.gradePercent) > 1e-7) return false;
    speed = int(std::lround(target.speedKmh / speedScale() * 10));
    for (const auto &point : inclines) {
        if (std::abs(point.percent - target.gradePercent) < 1e-7) { incline = point.code; return true; }
    }
    return false;
}
bool DeviceProfile::fromWire(int speed, int incline, Target &target) const {
    if (!validate().isEmpty() || speed < 0 || speed > maxSpeedCode) return false;
    for (const auto &point : inclines) {
        if (point.code == incline) { target = {speed * speedScale() / 10, point.percent}; return true; }
    }
    return false;
}
static bool keys(const QJsonObject &object, const QSet<QString> &allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) if (!allowed.contains(it.key())) return false;
    return true;
}
static bool integer(const QJsonObject &object, const QString &key, int &number) {
    const auto value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < -1 || value.toDouble() > 65535 ||
        std::floor(value.toDouble()) != value.toDouble()) return false;
    number = int(value.toDouble()); return true;
}
ProfileLoadResult loadProfile(const QString &path) {
    ProfileLoadResult result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    if (file.size() > 65536) { result.error = "Profile exceeds 64 KiB"; return result; }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) { result.error = "Invalid profile JSON"; return result; }
    const auto root = document.object();
    const auto identity = root.value("identity").toObject();
    const auto gatt = root.value("gatt").toObject();
    const auto speed = root.value("speed").toObject();
    const auto validation = root.value("validation").toObject();
    if (root.value("version").toDouble() != 1 || !keys(root, {"version", "identity", "gatt", "speed", "incline", "max_grade_mapping_loss", "validation"}) ||
        !keys(identity, {"address", "name", "model", "firmware"}) || !keys(gatt, {"service", "write", "notify"}) ||
        !keys(speed, {"unit", "min_code", "max_code", "running_min_code", "step_code", "unit_tag"}) ||
        !keys(validation, {"diagnostic_writes_confirmed", "controls", "speed_feedback", "incline_feedback", "stop_feedback", "pause", "evidence"})) {
        result.error = "Unsupported profile version/field"; return result;
    }
    auto &profile = result.profile;
    for (const QString key : {"address", "name", "model", "firmware"}) {
        if (!identity.value(key).isString()) { result.error = "Identity fields must be strings"; return result; }
    }
    profile.address = identity.value("address").toString().toUpper(); profile.name = identity.value("name").toString();
    profile.model = identity.value("model").toString(); profile.firmware = identity.value("firmware").toString();
    int *uuidFields[] = {&profile.service, &profile.write, &profile.notify};
    int index = 0;
    for (const QString key : {"service", "write", "notify"}) {
        const auto value = gatt.value(key);
        bool ok = false;
        const auto text = value.toString();
        const uint number = text.toUInt(&ok, 16);
        if (!value.isString() || text.size() != 4 || !ok || number > 65535) { result.error = "GATT UUIDs must be four-digit hex strings"; return result; }
        *uuidFields[index++] = int(number);
    }
    if (!integer(speed, "min_code", profile.minSpeedCode) || !integer(speed, "max_code", profile.maxSpeedCode) ||
        !integer(speed, "running_min_code", profile.runningMinCode) || !integer(speed, "step_code", profile.speedStepCode) ||
        !integer(speed, "unit_tag", profile.unitTag) || (speed.value("unit") != "km/h" && speed.value("unit") != "mph")) {
        result.error = "Explicit speed unit and integer code fields are required"; return result;
    }
    profile.speedUnit = speed.value("unit") == "mph" ? DeviceProfile::SpeedUnit::Mph : DeviceProfile::SpeedUnit::Kmh;
    if (!root.value("incline").isArray() || !root.value("max_grade_mapping_loss").isDouble()) {
        result.error = "Incline map and mapping tolerance are required"; return result;
    }
    profile.maxGradeMappingLoss = root.value("max_grade_mapping_loss").toDouble();
    for (const auto &value : root.value("incline").toArray()) {
        const auto point = value.toObject(); int code = -1;
        if (!value.isObject() || !keys(point, {"code", "percent"}) || !integer(point, "code", code) || !point.value("percent").isDouble()) {
            result.error = "Invalid incline mapping entry"; return result;
        }
        profile.inclines.append({code, point.value("percent").toDouble()});
    }
    bool *flags[] = {&profile.diagnosticsConfirmed, &profile.controlsVerified, &profile.speedFeedbackVerified,
                     &profile.inclineFeedbackVerified, &profile.stopFeedbackVerified, &profile.pauseVerified};
    index = 0;
    for (const QString key : {"diagnostic_writes_confirmed", "controls", "speed_feedback", "incline_feedback", "stop_feedback", "pause"}) {
        if (!validation.value(key).isBool()) { result.error = "Verification fields must be explicit booleans"; return result; }
        *flags[index++] = validation.value(key).toBool();
    }
    if (!validation.value("evidence").isString()) { result.error = "Evidence description is required"; return result; }
    profile.evidence = validation.value("evidence").toString();
    result.error = profile.validate();
    return result;
}
}
