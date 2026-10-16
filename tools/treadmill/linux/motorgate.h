#pragma once
// Helper-side boundary for W1 motor writes on the Linux backend. This is a port of
// MotorTestGate in windows-diagnostics.cs and must stay behaviorally identical to it:
// the CLI host has its own gates, and this one still refuses anything the host gets wrong.
// Only start-stop, speed-N, incline-N and workout plans are supported. Pause, READY and
// operator-timed trials are not available on the Linux backend.
//
// Two profiles. Walking (default) accepts only the reviewed walking-mode limits, raw speed 10-60 with unit
// tag 0. Running is opt-in, workouts only, and accepts only the 10-120 / tag 0x20 report the W1 has been seen
// to send; walking limits are refused in it and running limits are refused in walking, so a plan can never
// promote itself. The raw speed scale in running mode (raw 100 = 10.0 km/h) is not verified by any
// motor trial yet, so in the running profile a started run is stopped if the speed the treadmill reports over
// FTMS disagrees with its FitShow speed, or cannot be observed, while above 6 km/h.
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace Treadmill {

enum class Profile { Walking, Running };

class MotorGate {
  public:
    MotorGate(const QString &trial, const QString &targetSequence, int timeoutSeconds, Profile profile = Profile::Walking)
        : profile_(profile) {
        if (profile == Profile::Running && trial != "workout") throw std::invalid_argument("The running profile supports workouts only");
        targets_ = {targetFrame(10, 0)};
        const QStringList parts = trial.split('-');
        bool numeric = false;
        const int code = parts.size() == 2 ? parts[1].toInt(&numeric) : 0;
        if (trial == "workout") {
            if (targetSequence.isEmpty() || targetSequence.size() > 4096 || timeoutSeconds < 1 || timeoutSeconds > 9000)
                throw std::invalid_argument("Workout target sequence/deadline is outside the reviewed limit");
            const QStringList pairs = targetSequence.split(',');
            if (pairs.size() > 512) throw std::invalid_argument("Workout has too many target changes");
            targets_.clear();
            int previousSpeed = -1, previousIncline = -1;
            for (const QString &pair : pairs) {
                const QStringList fields = pair.split(':');
                bool speedOk = false, inclineOk = false;
                const int speed = fields.size() == 2 ? fields[0].toInt(&speedOk) : 0;
                const int incline = fields.size() == 2 ? fields[1].toInt(&inclineOk) : 0;
                if (!speedOk || !inclineOk || speed < 10 || speed > maxSpeedCode() || incline < 0 || incline > 8 ||
                    (speed == previousSpeed && incline == previousIncline))
                    throw std::invalid_argument("Workout targets must be distinct, validated W1 speed/incline codes");
                targets_.push_back(targetFrame(speed, incline));
                previousSpeed = speed; previousIncline = incline;
                if (speed > speedCeiling_) speedCeiling_ = speed;
                if (incline > inclineCeiling_) inclineCeiling_ = incline;
            }
            deadlineMs_ = timeoutSeconds * 1000;
        } else if (numeric && parts[0] == "speed" &&
                   (code == 12 || code == 15 || code == 20 || code == 30 || code == 40 || code == 50 || code == 60)) {
            speedCeiling_ = code; deadlineMs_ = 60000;
            targets_ = {targetFrame(10, 0), targetFrame(code, 0), targetFrame(10, 0)};
        } else if (numeric && parts[0] == "incline" && code >= 1 && code <= 8) {
            inclineCeiling_ = code; deadlineMs_ = 80000;
            targets_ = {targetFrame(10, 0), targetFrame(10, code), targetFrame(10, 0)};
        } else if (trial != "start-stop") {
            throw std::invalid_argument("Trial is not available on the Linux backend");
        }
    }

    bool started() const { return startedAt_ >= 0; }
    bool stopAttempted() const { return stopAttempted_; }

    // Validates the STX/ETX envelope and XOR checksum; returns the raw frame bytes.
    static QByteArray parse(const QString &hex) {
        if (hex.size() < 8 || hex.size() > 510 || hex.size() % 2 != 0) throw std::invalid_argument("Invalid frame length");
        for (const QChar c : hex)
            if (QStringLiteral("0123456789abcdef").indexOf(c.toLower()) < 0) throw std::invalid_argument("Invalid frame hex");
        const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
        quint8 checksum = 0;
        for (int i = 1; i < bytes.size() - 2; ++i) checksum ^= quint8(bytes[i]);
        if (quint8(bytes[0]) != 2 || quint8(bytes[bytes.size() - 1]) != 3 || quint8(bytes[bytes.size() - 2]) != checksum)
            throw std::invalid_argument("Invalid frame envelope/checksum");
        return bytes;
    }

    // Feeds one FTMS Treadmill Data (2ACD) notification; only the instantaneous speed is read.
    void observeFtms(const QByteArray &bytes, qint64 now) {
        if (bytes.size() < 4) return;
        const int flags = int(quint8(bytes[0])) | (int(quint8(bytes[1])) << 8);
        if (flags & 1) return; // "more data": speed is absent from this frame
        ftmsSpeedKmh_ = (int(quint8(bytes[2])) | (int(quint8(bytes[3])) << 8)) / 100.0;
        ftmsAt_ = now;
    }
    QString stopReason() const { return stopReason_; }

    // Feeds one FFF1 notification. Only checksum-valid frames of the lengths QZ uses refresh the clock.
    void observe(const QByteArray &bytes, qint64 now) {
        try { parse(QString::fromLatin1(bytes.toHex())); } catch (const std::exception &) { return; }
        if (bytes.size() < 5) return;
        auto at = [&](int i) { return int(quint8(bytes[i])); };
        if (at(1) == 0x50 && at(2) == 2 && bytes.size() >= 8) {
            minimum_ = at(4); maximum_ = at(3); unit_ = at(5);
        } else if (at(1) == 0x50 && at(2) == 3 && bytes.size() >= 7) {
            inclineMinimum_ = at(4); inclineMaximum_ = at(3);
        } else if (at(1) == 0x51) {
            const int state = at(2);
            const bool motorData = state == 1 || state == 3 || state == 4 || state == 10;
            if (motorData && bytes.size() < 16) return;
            if (state != 0 && state != 2 && !motorData && state != 5 && state != 6 && state != 7) return;
            statusAt_ = now;
            normalReports_ = state == 0 ? normalReports_ + 1 : 0;
            if (started() && (state == 5 || state == 6 || state == 7 || state == 10 || state == 1)) require("Treadmill reported fault, disabled or paused state");
            if (started() && motorData && (at(3) > speedCeiling_ || at(4) > inclineCeiling_ || at(4) > 127)) require("Unexpected motor feedback");
            if (started() && profile_ == Profile::Running && state == 3 && motorData && at(3) > 60) checkSpeedAgreement(at(3) / 10.0, now);
        }
        if (started() && !boundsMatch()) require("Limits changed during the run");
    }

    bool needsStop(qint64 now) const {
        return started() && !stopAttempted_ && (stopRequired_ || now - startedAt_ >= deadlineMs_ || now - statusAt_ > 1500);
    }

    // Returns the bytes to write, or throws if the frame is outside this trial's allowlist.
    QByteArray approve(const QString &hex, qint64 now) {
        const QByteArray bytes = parse(hex);
        const QString value = hex.toLower();
        if (value == "0250025203" || value == "0250035303" || value == "02515103") return bytes;
        if (value == "0253035003") {
            if (!started() || stopAttempted_) throw std::invalid_argument("Stop is available once after a start attempt");
            stopAttempted_ = true;
            return bytes;
        }
        if (value == "02530100000000000000005203") {
            if (started() || !boundsMatch() || normalReports_ < 3 || statusAt_ < 0 || now - statusAt_ > 1500)
                throw std::invalid_argument("Trial preflight requires reviewed walking bounds and three fresh permitted idle reports");
            startedAt_ = now; // Count the attempt even if the write's completion is uncertain.
            return bytes;
        }
        if (targetIndex_ < targets_.size() && value == targets_[targetIndex_]) {
            if (!started() || stopAttempted_ || needsStop(now))
                throw std::invalid_argument("Target is unavailable after stop, stale status or trial deadline");
            ++targetIndex_;
            return bytes;
        }
        throw std::invalid_argument("Frame is outside the bounded motor trial allowlist");
    }

  private:
    static QString targetFrame(int speed, int incline) {
        const QByteArray bytes{QByteArray::fromRawData("\x02\x53\x02", 3) + char(speed) + char(incline) +
                               char(0x53 ^ 2 ^ speed ^ incline) + char(3)};
        return QString::fromLatin1(bytes.toHex());
    }
    int maxSpeedCode() const { return profile_ == Profile::Running ? 120 : 60; }
    int unitTag() const { return profile_ == Profile::Running ? 0x20 : 0; }
    bool boundsMatch() const {
        return minimum_ == 10 && maximum_ == maxSpeedCode() && unit_ == unitTag() && inclineMinimum_ == 0 && inclineMaximum_ == 8;
    }
    void require(const QString &reason) { stopRequired_ = true; if (stopReason_.isEmpty()) stopReason_ = reason; }
    // Above 6 km/h the raw speed scale is unverified: FTMS must be observable and must agree. The two streams
    // have no guaranteed order, so missing or stale FTMS speed stops the run only if it persists for 2 s.
    void checkSpeedAgreement(double fitshowKmh, qint64 now) {
        if (ftmsAt_ < 0 || now - ftmsAt_ > 3000) {
            if (ftmsMissingSince_ < 0) ftmsMissingSince_ = now;
            if (now - ftmsMissingSince_ >= 2000) require("FTMS speed cannot be observed; the raw speed scale is unverified");
            return;
        }
        ftmsMissingSince_ = -1;
        if (std::fabs(ftmsSpeedKmh_ - fitshowKmh) > 0.6) {
            if (++speedMismatch_ >= 4) require(QString("FTMS speed %1 km/h disagrees with FitShow speed %2 km/h").arg(ftmsSpeedKmh_).arg(fitshowKmh));
        } else {
            speedMismatch_ = 0;
        }
    }

    int minimum_ = -1, maximum_ = -1, unit_ = -1, inclineMinimum_ = -1, inclineMaximum_ = -1;
    int normalReports_ = 0;
    qint64 statusAt_ = -1, startedAt_ = -1;
    bool stopRequired_ = false, stopAttempted_ = false;
    Profile profile_ = Profile::Walking;
    double ftmsSpeedKmh_ = 0;
    qint64 ftmsAt_ = -1, ftmsMissingSince_ = -1;
    int speedMismatch_ = 0;
    QString stopReason_;
    int speedCeiling_ = 10, inclineCeiling_ = 0, deadlineMs_ = 12000;
    std::vector<QString> targets_;
    size_t targetIndex_ = 0;
};

} // namespace Treadmill
