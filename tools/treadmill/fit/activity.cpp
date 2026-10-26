#include "activity.h"
#include "fitworkout.h"
#include <QDateTime>
#include <cmath>
#include <sstream>
#include "fit_activity_mesg.hpp"
#include "fit_encode.hpp"
#include "fit_event_mesg.hpp"
#include "fit_file_id_mesg.hpp"
#include "fit_lap_mesg.hpp"
#include "fit_record_mesg.hpp"
#include "fit_session_mesg.hpp"

namespace Treadmill {
namespace Fit {
namespace {
constexpr qint64 kFitEpochOffset = 631065600; // seconds from 1970-01-01 to 1989-12-31
struct Status { qint64 ms; int state; int speedTenths; int level; };
struct Odometer { qint64 ms; qint64 meters; };

// FTMS Treadmill Data (2ACD): total distance (metres, 3 bytes) follows the speed fields when flags bit 2 is set.
bool ftmsTotalDistance(const QByteArray &bytes, qint64 &meters) {
    if (bytes.size() < 2) return false;
    const int flags = int(quint8(bytes[0])) | (int(quint8(bytes[1])) << 8);
    int offset = 2;
    if (!(flags & 1)) offset += 2;      // instantaneous speed
    if (flags & 2) offset += 2;         // average speed
    if (!(flags & 4) || bytes.size() < offset + 3) return false;
    meters = int(quint8(bytes[offset])) | (int(quint8(bytes[offset + 1])) << 8) | (int(quint8(bytes[offset + 2])) << 16);
    return true;
}
}

QByteArray buildActivity(const QVector<QJsonObject> &rows, const QString &sport, ActivitySummary &summary, QString &error) {
    summary = ActivitySummary();
    if (sport != "hiking" && sport != "walking" && sport != "running") { error = "sport must be hiking, walking or running"; return {}; }
    QVector<Status> statuses;
    qint64 anchorMs = -1, anchorUnixMs = -1;
    for (const QJsonObject &row : rows) {
        if (row.value("event").toString() != "raw_status") continue;
        const int state = row.value("status_code").toInt(-1);
        // NORMAL and countdown between running periods (a console pause) carry no motor data: the belt is stopped.
        if ((state == 0 || state == 2) && !statuses.isEmpty()) {
            statuses.append({qint64(row.value("monotonic_ms").toDouble(-1)), state, 0, statuses.last().level});
            continue;
        }
        if (!row.value("has_motor_data").toBool()) continue;
        const int speed = row.value("speed_code").toInt(-1), level = row.value("incline_code").toInt(-1);
        if ((state != 3 && state != 4) || speed < 0 || level < 0 || level > kMaxLevel) continue;
        const qint64 ms = qint64(row.value("monotonic_ms").toDouble(-1));
        if (ms < 0) continue;
        if (anchorMs < 0) {
            const QDateTime utc = QDateTime::fromString(row.value("utc").toString(), Qt::ISODateWithMs);
            if (!utc.isValid()) { error = "log rows need ISO 'utc' timestamps"; return {}; }
            anchorMs = ms; anchorUnixMs = utc.toMSecsSinceEpoch();
        }
        statuses.append({ms, state, speed, level});
    }
    int first = -1;
    for (int i = 0; i < statuses.size(); ++i) if (statuses[i].state == 3) { first = i; break; }
    if (first < 0) { error = "the log contains no RUNNING status from the treadmill"; return {}; }
    while (statuses.last().state != 3 && statuses.last().state != 4) statuses.removeLast(); // NORMAL after the final stop
    const qint64 startMs = statuses[first].ms, endMs = statuses.last().ms;
    if (endMs <= startMs) { error = "the log's running period is empty"; return {}; }
    const qint64 startUnix = (anchorUnixMs + (startMs - anchorMs)) / 1000;
    const int seconds = int((endMs - startMs + 999) / 1000);

    // The treadmill's own distance counter (FTMS), when the log has one covering the run; otherwise distance is
    // integrated from the reported speed. The counter restarts at zero after a stop and after a console pause, so a
    // falling value starts a new segment that adds to the ones before.
    QVector<Odometer> odometer;
    for (const QJsonObject &row : rows) {
        if (row.value("event").toString() != "ftms_notification" ||
            row.value("characteristic_uuid").toString() != "00002acd-0000-1000-8000-00805f9b34fb") continue;
        const qint64 ms = qint64(row.value("monotonic_ms").toDouble(-1));
        qint64 meters = 0;
        if (ms >= startMs && ms <= endMs && ftmsTotalDistance(QByteArray::fromHex(row.value("frame_hex").toString().toLatin1()), meters))
            odometer.append({ms, meters});
    }
    qint64 added = 0;
    for (int i = 1; i < odometer.size(); ++i) { // make the counter cumulative across restarts
        if (odometer[i].meters + added < odometer[i - 1].meters) added = odometer[i - 1].meters;
        odometer[i].meters += added;
    }
    const bool reported = odometer.size() >= 2;

    std::vector<fit::RecordMesg> records;
    double distance = 0, estimatedDistance = 0, altitude = 0, maxSpeed = 0, speedSum = 0;
    int cursor = first, odometerCursor = 0;
    for (int k = 0; k <= seconds; ++k) {
        while (cursor + 1 < statuses.size() && statuses[cursor + 1].ms <= startMs + qint64(k) * 1000) ++cursor;
        const Status &now = statuses[cursor];
        const double speedMps = now.speedTenths / 10.0 / 3.6, grade = now.level * kGradePerLevel;
        if (k > 0) { estimatedDistance += speedMps; altitude += speedMps * grade / 100.0; }
        if (reported) {
            while (odometerCursor + 1 < odometer.size() && odometer[odometerCursor + 1].ms <= startMs + qint64(k) * 1000) ++odometerCursor;
            distance = std::max(distance, double(odometer[odometerCursor].meters - odometer[0].meters));
        } else distance = estimatedDistance;
        maxSpeed = std::max(maxSpeed, speedMps);
        speedSum += speedMps;
        fit::RecordMesg record;
        record.SetTimestamp(FIT_DATE_TIME(startUnix + k - kFitEpochOffset));
        record.SetDistance(FIT_FLOAT32(distance));
        record.SetSpeed(FIT_FLOAT32(speedMps));
        record.SetEnhancedSpeed(FIT_FLOAT32(speedMps));
        record.SetAltitude(FIT_FLOAT32(altitude));
        record.SetEnhancedAltitude(FIT_FLOAT32(altitude));
        record.SetGrade(FIT_FLOAT32(grade));
        records.push_back(record);
    }
    summary.seconds = seconds; summary.distanceMeters = distance; summary.distanceReported = reported; summary.ascentMeters = altitude;
    summary.maxSpeedKmh = maxSpeed * 3.6; summary.records = int(records.size());
    const double avgSpeed = speedSum / records.size();
    const FIT_DATE_TIME start = FIT_DATE_TIME(startUnix - kFitEpochOffset), end = FIT_DATE_TIME(startUnix + seconds - kFitEpochOffset);
    const FIT_SPORT fitSport = sport == "walking" ? FIT_SPORT_WALKING : sport == "running" ? FIT_SPORT_RUNNING : FIT_SPORT_HIKING;

    fit::FileIdMesg fileId;
    fileId.SetType(FIT_FILE_ACTIVITY);
    fileId.SetManufacturer(FIT_MANUFACTURER_DEVELOPMENT);
    fileId.SetProduct(1);
    fileId.SetSerialNumber(1);
    fileId.SetTimeCreated(start);
    fit::EventMesg startEvent;
    startEvent.SetTimestamp(start);
    startEvent.SetEvent(FIT_EVENT_TIMER);
    startEvent.SetEventType(FIT_EVENT_TYPE_START);
    fit::EventMesg stopEvent;
    stopEvent.SetTimestamp(end);
    stopEvent.SetEvent(FIT_EVENT_TIMER);
    stopEvent.SetEventType(FIT_EVENT_TYPE_STOP_ALL);
    fit::LapMesg lap;
    lap.SetMessageIndex(0);
    lap.SetTimestamp(end);
    lap.SetStartTime(start);
    lap.SetTotalElapsedTime(FIT_FLOAT32(seconds));
    lap.SetTotalTimerTime(FIT_FLOAT32(seconds));
    lap.SetTotalDistance(FIT_FLOAT32(distance));
    lap.SetTotalAscent(FIT_UINT16(std::lround(altitude)));
    lap.SetTotalDescent(0);
    lap.SetAvgSpeed(FIT_FLOAT32(avgSpeed));
    lap.SetEnhancedAvgSpeed(FIT_FLOAT32(avgSpeed));
    lap.SetMaxSpeed(FIT_FLOAT32(maxSpeed));
    lap.SetEnhancedMaxSpeed(FIT_FLOAT32(maxSpeed));
    lap.SetSport(fitSport);
    lap.SetSubSport(FIT_SUB_SPORT_GENERIC);
    lap.SetEvent(FIT_EVENT_LAP);
    lap.SetEventType(FIT_EVENT_TYPE_STOP);
    lap.SetLapTrigger(FIT_LAP_TRIGGER_SESSION_END);
    fit::SessionMesg session;
    session.SetMessageIndex(0);
    session.SetTimestamp(end);
    session.SetStartTime(start);
    session.SetTotalElapsedTime(FIT_FLOAT32(seconds));
    session.SetTotalTimerTime(FIT_FLOAT32(seconds));
    session.SetTotalDistance(FIT_FLOAT32(distance));
    session.SetTotalAscent(FIT_UINT16(std::lround(altitude)));
    session.SetTotalDescent(0);
    session.SetAvgSpeed(FIT_FLOAT32(avgSpeed));
    session.SetEnhancedAvgSpeed(FIT_FLOAT32(avgSpeed));
    session.SetMaxSpeed(FIT_FLOAT32(maxSpeed));
    session.SetEnhancedMaxSpeed(FIT_FLOAT32(maxSpeed));
    session.SetMaxAltitude(FIT_FLOAT32(altitude));
    session.SetSport(fitSport);
    session.SetSubSport(FIT_SUB_SPORT_GENERIC); // not "treadmill": watches may drop altitude for treadmill activities
    session.SetSportProfileName(L"Treadmill W1");
    session.SetFirstLapIndex(0);
    session.SetNumLaps(1);
    session.SetEvent(FIT_EVENT_SESSION);
    session.SetEventType(FIT_EVENT_TYPE_STOP);
    session.SetTrigger(FIT_SESSION_TRIGGER_ACTIVITY_END);
    fit::ActivityMesg activity;
    activity.SetTimestamp(end);
    activity.SetTotalTimerTime(FIT_FLOAT32(seconds));
    activity.SetNumSessions(1);
    activity.SetType(FIT_ACTIVITY_MANUAL);
    activity.SetEvent(FIT_EVENT_ACTIVITY);
    activity.SetEventType(FIT_EVENT_TYPE_STOP);
    activity.SetLocalTimestamp(end);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    fit::Encode encoder(fit::ProtocolVersion::V20);
    encoder.Open(stream);
    encoder.Write(fileId);
    encoder.Write(startEvent);
    for (const auto &record : records) encoder.Write(record);
    encoder.Write(stopEvent);
    encoder.Write(lap);
    encoder.Write(session);
    encoder.Write(activity);
    if (!encoder.Close()) { error = "FIT encoder failed to finish the file"; return {}; }
    const std::string bytes = stream.str();
    return QByteArray(bytes.data(), int(bytes.size()));
}

} // namespace Fit
} // namespace Treadmill
