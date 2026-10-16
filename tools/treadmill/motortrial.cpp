#include "motortrial.h"
#include "windowsbackend.h"
#include "terminal.h"
#include "fitshowcontrol.h"
#include "workout.h"
#include <QBluetoothAddress>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <cmath>
#include <utility>

static QByteArray frame(const QByteArray &payload) {
    return FitShowPacket::encode(reinterpret_cast<const uint8_t *>(payload.constData()), payload.size());
}
struct Trial {
    QString id;
    int speed = 10, incline = 0, stepHold = 5, returnHold = 0, deadline = 12;
    bool pause = false;
    bool operatorMarks = false;
    int arrivalHoldSeconds = 2;
    bool readyOnly = false;
    bool sleepReadyAllowed = false;
    struct Step { int speed = 10, incline = 0, holdSeconds = 1; QString name; };
    bool workout = false;
    QString workoutId, workoutName;
    QVector<Step> steps;
    Trial() = default;
    Trial(QString trialId, int speedCode = 10, int inclineCode = 0, int stepSeconds = 5,
          int returnSeconds = 0, int deadlineSeconds = 12, bool pauseTrial = false,
          bool marks = false, int arrivalSeconds = 2, bool ready = false, bool sleepReady = false)
        : id(std::move(trialId)), speed(speedCode), incline(inclineCode), stepHold(stepSeconds),
          returnHold(returnSeconds), deadline(deadlineSeconds), pause(pauseTrial), operatorMarks(marks),
          arrivalHoldSeconds(arrivalSeconds), readyOnly(ready), sleepReadyAllowed(sleepReady) {}
};
static QVector<Trial> preparedTrials() {
    QVector<Trial> trials {{"start-stop"}};
    for (int code : {12, 15, 20, 30, 40, 50, 60})
        trials.append({"speed-" + QString::number(code), code, 0, code == 12 ? 3 : 5, 3, 60, false});
    for (int code = 1; code <= 8; ++code)
        trials.append({"incline-" + QString::number(code), 10, code, 15, 15, 80, false});
    trials.append({"pause", 10, 0, 5, 0, 18, true});
    trials.append({"incline-timing-4", 10, 4, 60, 60, 150, false, true});
    trials.append({"incline-timing-8", 10, 8, 60, 60, 150, false, true});
    trials.append({"incline-measure-4", 10, 4, 60, 60, 210, false, true, 30});
    trials.append({"incline-measure-8", 10, 8, 60, 60, 210, false, true, 30});
    trials.append({"ready-idle", 10, 0, 15, 0, 30, false, false, 2, true});
    trials.append({"ready-wake", 10, 0, 15, 0, 30, false, false, 2, true, true});
    return trials;
}
static QByteArray targetPayload(int speed, int incline) {
    QByteArray payload = QByteArray::fromHex("5302");
    payload.append(char(speed)); payload.append(char(incline));
    return payload;
}

// The Linux transport is a separate helper process that speaks the same line protocol as the Windows helper.
static QString linuxHelperPath(QString &error) {
    QStringList candidates;
    if (qEnvironmentVariableIsSet("TREADMILL_LINUX_HELPER")) candidates << qEnvironmentVariable("TREADMILL_LINUX_HELPER");
    const QDir binDir(QCoreApplication::applicationDirPath());
    candidates << binDir.absoluteFilePath("../libexec/treadmill/treadmill-linux-helper") << binDir.absoluteFilePath("treadmill-linux-helper");
    for (const QString &candidate : candidates)
        if (QFileInfo(candidate).isExecutable()) return QFileInfo(candidate).absoluteFilePath();
    error = "Linux Bluetooth helper is missing; build tools/treadmill/linux/linux-helper.pro or set TREADMILL_LINUX_HELPER";
    return {};
}

static int runMotorTrial(QCoreApplication &app, const QCommandLineParser &parser,
                         const QString &trialOverride, bool batchConfirmed, const QString &logPath,
                         const Trial *workoutOverride = nullptr) {
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    const bool linuxBackend = parser.value("backend") == "linux";
    if (!linuxBackend && parser.value("backend") != "windows") return fail("The bounded control trial requires --backend windows or --backend linux");
    const auto trials = preparedTrials();
    if (parser.isSet("list-trials")) {
        for (const QString option : {"device", "expected-name", "address-type", "dry-run", "confirm-motor", "log", "trial", "timeout", "simulate", "fast", "profile", "confirm-diagnostics", "workouts-dir", "inject", "inject-at"})
            if (parser.isSet(option)) return fail("--list-trials does not accept " + option);
        QJsonArray catalog;
        for (const auto &trial : trials) catalog.append(QJsonObject {{"id", trial.id}, {"speed_ceiling_code", trial.speed},
            {"incline_ceiling_code", trial.incline}, {"step_hold_seconds", trial.stepHold}, {"return_hold_seconds", trial.returnHold},
            {"windows_motor_deadline_seconds", trial.deadline}, {"pause", trial.pause}, {"operator_marks_required", trial.operatorMarks}, {"arrival_hold_seconds", trial.arrivalHoldSeconds}, {"ready_only", trial.readyOnly}, {"sleep_ready_allowed", trial.sleepReadyAllowed}, {"motor_execution_enabled", false}});
        QTextStream(stdout) << QJsonDocument(catalog).toJson();
        return 0;
    }
    Trial trial;
    bool found = workoutOverride != nullptr;
    if (workoutOverride) trial = *workoutOverride;
    const QString trialId = trialOverride.isEmpty() ? parser.value("trial") : trialOverride;
    if (!found) for (const auto &candidate : trials) if (candidate.id == trialId) { trial = candidate; found = true; break; }
    if (!found) return fail("Unknown trial; use control-test --backend windows --list-trials");
    if (linuxBackend && (trial.pause || trial.readyOnly || trial.operatorMarks))
        return fail("Pause, READY and operator-timed trials are not available on the Linux backend");
    const QBluetoothAddress address(parser.value("device"));
    const QString name = parser.value("expected-name");
    if (address.isNull() || name.trimmed().isEmpty()) return fail("Control trial requires --device and exact --expected-name");
    // Wait-for-wake: poll without any motor command until the console is awake, then run the normal preflight.
    // Unattended mode starts by itself after a grace period; confirm mode starts only on ENTER.
    bool waitOk = true;
    const double waitMinutes = parser.isSet("wait-wake") ? parser.value("wait-wake").toDouble(&waitOk) : 0;
    const bool waitConfirm = parser.isSet("wait-wake-confirm");
    if (parser.isSet("wait-wake") && (!waitOk || waitMinutes < 0.05 || waitMinutes > 30)) return fail("--wait-wake must be 0.05-30 minutes");
    if (waitConfirm && !parser.isSet("wait-wake")) return fail("--wait-wake-confirm requires --wait-wake");
    if ((parser.isSet("wait-wake") || waitConfirm) && !linuxBackend) return fail("--wait-wake is only available on the Linux backend");
    const qint64 waitWakeMs = qint64(std::llround(waitMinutes * 60000.0));
    constexpr qint64 kWakeGraceMs = 5000;
    for (const QString option : {"simulate", "fast", "workouts-dir", "inject", "inject-at", "profile", "confirm-diagnostics", "timeout"})
        if (parser.isSet(option)) return fail("Unsupported control trial option: " + option);
    QJsonArray sequence;
    QVector<QByteArray> payloads {QByteArray::fromHex("53010000000000000000"), targetPayload(10, 0)};
    QString targetSequence;
    if (trial.workout) {
        payloads.clear();
        payloads.append(QByteArray::fromHex("53010000000000000000"));
        payloads.append(targetPayload(10, 0));
        int previousSpeed = 10, previousIncline = 0;
        targetSequence = "10:0";
        for (const auto &step : trial.steps) {
            if (step.speed == previousSpeed && step.incline == previousIncline) continue;
            payloads.append(targetPayload(step.speed, step.incline));
            if (!targetSequence.isEmpty()) targetSequence.append(',');
            targetSequence += QString::number(step.speed) + ':' + QString::number(step.incline);
            previousSpeed = step.speed; previousIncline = step.incline;
        }
    }
    if (trial.readyOnly) payloads = {QByteArray::fromHex("53010000000080000000")};
    if (!trial.workout && trial.returnHold > 0) payloads << targetPayload(trial.speed, trial.incline) << targetPayload(10, 0);
    if (trial.pause) payloads << QByteArray::fromHex("5306");
    payloads << QByteArray::fromHex("5303");
    for (const auto &payload : payloads)
        sequence.append(QString::fromLatin1(frame(payload).toHex()));
    QJsonArray workoutSteps;
    if (trial.workout) {
        for (int i = 0; i < trial.steps.size(); ++i) {
            const auto &step = trial.steps[i];
            workoutSteps.append(QJsonObject{{"index", i}, {"name", step.name}, {"hold_seconds", step.holdSeconds},
                {"speed_code", step.speed}, {"speed_kmh_assumption", step.speed / 10.0},
                {"incline_code", step.incline}, {"grade_percent_assumption", step.incline * (11.0 / 8.0)}});
        }
    }
    const QJsonObject plan {{"backend", linuxBackend ? "linux-bluez" : "windows-winrt"}, {"device", address.toString()}, {"expected_name", name},
                           {"motor_execution_enabled", false}, {"speed_code", 10}, {"incline_code", 0},
                           {"trial", trial.id}, {"workout_id", trial.workoutId}, {"workout_name", trial.workoutName},
                           {"step_count", trial.steps.size()}, {"speed_ceiling_code", trial.speed}, {"incline_ceiling_code", trial.incline},
                           {"initial_hold_seconds", trial.workout ? trial.steps.first().holdSeconds : (trial.readyOnly ? trial.stepHold : (trial.id == "start-stop" ? 5 : 3))},
                           {"step_hold_seconds", trial.stepHold}, {"workout_steps", workoutSteps},
                           {"return_hold_seconds", trial.returnHold}, {"windows_motor_deadline_seconds", trial.deadline},
                           {"operator_marks_required", trial.operatorMarks}, {"arrival_hold_seconds", trial.arrivalHoldSeconds}, {"ready_only", trial.readyOnly}, {"sleep_ready_allowed", trial.sleepReadyAllowed}, {"expected_ready_status", trial.readyOnly ? QJsonValue(9) : QJsonValue(QJsonValue::Null)}, {"physical_arrival_source", "operator observation only"},
                           {"wait_wake_seconds", waitWakeMs / 1000.0}, {"wait_wake_confirm", waitConfirm},
                           {"initial_deck_baseline_confirmation_required", trial.operatorMarks},
                           {"ftms_observation_requested", parser.isSet("observe-ftms")},
                           {"frames", sequence}, {"physical_units_verified", false}, {"physical_stop_verified", false},
                           {"preflight", trial.sleepReadyAllowed
                                ? "raw limits 10/60/unit 0 and incline 0/8; three fresh NORMAL or sleep 06/02 replies plus operator-confirmed stopped belt; key 06/01 and unknown disabled reasons refuse"
                                : "raw limits 10/60/unit 0 and incline 0/8; three fresh NORMAL replies plus operator-confirmed stopped belt"}};
    if (parser.isSet("dry-run")) {
        if (parser.isSet("confirm-motor") || parser.isSet("log")) return fail("Dry run does not accept --confirm-motor or --log");
        QTextStream(stdout) << QJsonDocument(plan).toJson();
        return 0;
    }
    if (trial.readyOnly) return fail("READY trials retired: awake W1 entered countdown and stop was not verified. No Bluetooth I/O.");
    if ((!batchConfirmed && !parser.isSet("confirm-motor")) || !parser.isSet("log")) return fail("Control trial requires --confirm-motor and a new --log; inspect --dry-run first");
    if (!isatty(STDIN_FILENO)) return fail("Motor trial requires an interactive operator confirmation");
    if (!batchConfirmed) {
    int nominalSeconds = 0;
    if (trial.workout) for (const auto &step : trial.steps) nominalSeconds += step.holdSeconds;
    const int longRunMinutes = nominalSeconds / 60;
    QTextStream(stderr) << QJsonDocument(plan).toJson()
        << (trial.workout ? "This runs the prepared W1 target schedule, holds each reported step, and explicitly stops. Incline percentages use the provisional linear assumption.\n"
                           : trial.readyOnly ? "This requests READY with mode bit 0x80, observes 15 seconds, then explicitly stops.\nW1 support is unverified: countdown or running triggers immediate stop.\n"
                           : "This starts at raw 10/0, follows only the printed trial sequence, and explicitly stops.\n")
        << "Holds begin at matching reported targets; attained incline and calibrated units remain unverified.\n"
        << (linuxBackend ? "The Linux helper" : "Windows") << " attempts stop on EOF, faults, stale status, or the printed motor deadline.\n"
        << "A disconnected Bluetooth link cannot guarantee delivery.\n"
           "Confirm the belt is stopped and clear, safety key normally attached, other controllers disconnected,\n"
           "and you are at the physical controls with an accessible stop.\n"
        << (waitWakeMs <= 0 ? QString() : waitConfirm
            ? QString("WAIT FOR WAKE: no motor command is sent until the treadmill is awake (up to %1 s); then press ENTER to start, or Q to cancel.\n").arg(waitWakeMs / 1000)
            : QString("WAIT FOR WAKE, AUTO-START: nothing is sent to the motor until the treadmill is awake (up to %1 s); then the belt STARTS BY ITSELF after %2 s unless you press Q.\n").arg(waitWakeMs / 1000).arg(kWakeGraceMs / 1000))
        << (longRunMinutes >= 60 ? QString("LONG RUN (%1 min): there is no pause and no temperature reading. The longest proven run is 62 minutes; check the motor housing by hand and stop if it is hot.\n").arg(longRunMinutes) : QString())
        << "Type MOTOR to approve this exact trial: " << Qt::flush;
    if (QTextStream(stdin).readLine() != "MOTOR") return fail("Motor trial was not confirmed");
    }
    if (trial.operatorMarks && batchConfirmed) return fail("Operator-timed trials must run individually");
    if (trial.operatorMarks) QTextStream(stderr) << "Start only with the deck physically settled at level 0.\n"
        "Timing keys: B when the deck begins moving, U when fully raised, D when fully returned.\n"
        "Mark arrival when the deck has visibly stopped moving. Each direction allows 60 seconds, then faults/stops.\n"
        "Q/S always aborts. Key timestamps are operator observations with reaction-time uncertainty.\n" << Qt::flush;
    QString helperError;
    const auto helper = linuxBackend ? linuxHelperPath(helperError) : windowsHelperPath(helperError);
    if (!helperError.isEmpty()) return fail(helperError);
    QFile log(logPath.isEmpty() ? parser.value("log") : logPath);
    if (!log.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail("Cannot create motor log: " + log.errorString());
    InputGuard input(true);
    if (!input.valid()) return fail("Cannot install motor trial signal handling");
    QElapsedTimer clock;
    clock.start();
    FitShow::Control control;
    QProcess process;
    QByteArray buffer;
    int normalCount = 0, postStopNormals = 0;
    bool started = false, stopping = false, closing = false, failed = false, stopWritten = false;
    bool feedbackAfterStop = false, hasFinalReport = false, ready = false, identityObserved = false;
    qint64 startedAt = -1, runningAt = -1, stoppedAt = -1, latestStatusAt = -1;
    bool sleepNoticed = false, startPrompted = false, startConfirmed = false;
    qint64 graceUntil = -1, lastWaitNoticeAt = 0;
    int stage = 0;
    int targetMatchCount = 0;
    bool workoutInitialTargetSent = false;
    qint64 stageReportedAt = -1, stageRequestedAt = -1, pauseAt = -1;
    qint64 operatorBeginAt[3] = {-1, -1, -1}, operatorArrivalAt[3] = {-1, -1, -1};
    QTimer closeDeadline;
    closeDeadline.setSingleShot(true);
    std::function<void()> close;
    auto record = [&](const QString &event, QJsonObject data) {
        data.insert("event", event); data.insert("monotonic_ms", clock.elapsed());
        data.insert("utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
        QTextStream(stdout) << bytes << Qt::flush;
        if (log.size() + bytes.size() > 64 * 1024 * 1024 || log.write(bytes) != bytes.size() || !log.flush()) {
            failed = true;
            QTextStream(stderr) << "Motor recording failed; Windows helper is being asked to stop.\n";
            if (close) close();
        }
    };
    close = [&]() {
        if (closing) return;
        closing = true;
        control.cancel();
        process.write("QUIT\n"); process.closeWriteChannel();
        closeDeadline.start(parser.isSet("observe-ftms") ? 10000 : 5000);
    };
    auto stop = [&](const QString &reason) {
        if (stopping || closing) return;
        stopping = true; stoppedAt = clock.elapsed();
        record("stop_requested", {{"reason", reason}});
        if (started && !closing) control.stop(1, 3);
        else close();
    };
    QObject::connect(&closeDeadline, &QTimer::timeout, &app, [&]() {
        failed = true;
        QTextStream(stderr) << "Helper shutdown timed out; use physical stop. Stop/cleanup are unconfirmed.\n";
        process.kill();
    });
    QObject::connect(&control, &FitShow::Control::payloadReady, &app, [&](const QByteArray &payload, quint64, quint64 id) {
        if (closing) return;
        const auto hex = frame(payload).toHex();
        record("write_requested", {{"command_id", QString::number(id)}, {"frame_hex", QString::fromLatin1(hex)}});
        if (!closing) process.write("WRITE\t" + QByteArray::number(id) + '\t' + hex + '\n');
    });
    QObject::connect(&control, &FitShow::Control::commandEvent, &app, [&](quint64 id, const QString &phase) {
        record("command", {{"command_id", QString::number(id)}, {"phase", phase}});
    });
    QObject::connect(&control, &FitShow::Control::fault, &app, [&](const QString &reason) {
        failed = true; record("fault", {{"reason", reason}});
        if (stopping) close(); else stop(reason);
    });
    auto observeStatus = [&](FitShow::Status status) {
        latestStatusAt = clock.elapsed();
        record("raw_status", {{"status_code", status.state}, {"has_motor_data", status.hasMotorData},
                              {"speed_code", status.speedTenths}, {"incline_code", status.inclineCode}, {"disabled_reason_code", status.disabledReason}});
        if (closing) return;
        if (!started) {
            normalCount = (status.state == 0 || (trial.sleepReadyAllowed && status.state == 6 && status.disabledReason == 2))
                ? normalCount + 1 : 0;
            const auto limits = control.limits();
            if (waitWakeMs > 0 && normalCount == 0 && (startPrompted || graceUntil >= 0 || startConfirmed)) {
                // Went back to sleep (or left NORMAL) before the start: an earlier ENTER no longer counts,
                // and the wake handshake begins again.
                startPrompted = false; startConfirmed = false; graceUntil = -1;
                record("wake_lost", {{"status_code", status.state}});
                QTextStream(stderr) << "Treadmill is no longer awake; waiting again.\n" << Qt::flush;
            }
            if (normalCount >= 3 && limits.complete() && waitWakeMs > 0 && !startConfirmed) {
                if (waitConfirm) {
                    if (!startPrompted) {
                        startPrompted = true;
                        record("awake_waiting_for_confirmation", {});
                        QTextStream(stderr) << "Treadmill is awake. Press ENTER to start, or Q to cancel.\n" << Qt::flush;
                    }
                    return;
                }
                if (graceUntil < 0) {
                    graceUntil = clock.elapsed() + kWakeGraceMs;
                    record("wake_grace_started", {{"grace_ms", kWakeGraceMs}});
                    QTextStream(stderr) << "Treadmill is awake. Starting in " << kWakeGraceMs / 1000 << " s; press Q to cancel.\n" << Qt::flush;
                }
                if (clock.elapsed() < graceUntil) return;
            }
            if (normalCount >= 3 && limits.complete()) {
                if (limits.minSpeed != 10 || limits.maxSpeed != 60 || limits.unit != 0 || limits.minIncline != 0 || limits.maxIncline != 8) {
                    failed = true; record("fault", {{"reason", "Live limits differ from the reviewed walking-mode trial"}}); close(); return;
                }
                if (trial.pause && !limits.pause) { failed = true; record("fault", {{"reason", "Pause is not advertised"}}); close(); return; }
                if (!control.arm(1)) { failed = true; close(); return; }
                started = true; startedAt = clock.elapsed();
                stageRequestedAt = startedAt;
                const int initialSpeed = 10;
                const int initialIncline = 0;
                record(trial.readyOnly ? "ready_requested" : "start_requested", {{"speed_code", initialSpeed}, {"incline_code", initialIncline}});
                if (!closing) {
                    if (trial.readyOnly) control.prepareReady(1, 1);
                    else control.start(1, 1, initialSpeed, initialIncline);
                }
            }
            return;
        }
        if (trial.readyOnly && !stopping) {
            if (trial.sleepReadyAllowed && status.state == 6 && status.disabledReason == 2 &&
                stageReportedAt < 0 && clock.elapsed() - startedAt < 2000) return;
            if (status.state != 0 && status.state != 9) {
                failed = true; stop("READY experiment received countdown, motion or unexpected state"); return;
            }
            if (status.state == 0 && stageReportedAt >= 0) {
                failed = true; stop("READY state cleared before the observation completed"); return;
            }
            if (status.state == 9 && stageReportedAt < 0) {
                stageReportedAt = clock.elapsed();
                record("reported_ready", {{"status_code", 9}, {"physical_stationary_verified", false}});
            }
        }
        if (!stopping && status.hasMotorData) {
            if (status.inclineCode < 0 || status.inclineCode > trial.incline || status.speedTenths > trial.speed || status.state == 1 ||
                ((status.state == 10 || status.state == 4) && pauseAt < 0)) {
                failed = true; stop("Unexpected motor feedback"); return;
            }
            if (status.state == 3 && status.speedTenths == 10 && runningAt < 0) {
                runningAt = clock.elapsed(); record("reported_running", {{"physical_movement_verified", false}});
                if (trial.workout && (trial.steps.first().speed != 10 || trial.steps.first().incline != 0)) {
                    const auto &initial = trial.steps.first();
                    stageRequestedAt = runningAt;
                    workoutInitialTargetSent = true;
                    record("target_requested", {{"stage", 0}, {"speed_code", initial.speed},
                                                  {"incline_code", initial.incline}, {"stage_name", initial.name}});
                    if (!closing) control.target(1, 2, initial.speed, initial.incline);
                } else if (trial.workout) workoutInitialTargetSent = true;
            }
            const int expectedSpeed = trial.workout ? (stage == 0 && !workoutInitialTargetSent ? 10 : trial.steps[qBound(0, stage, trial.steps.size() - 1)].speed) : (stage == 1 ? trial.speed : 10);
            const int expectedIncline = trial.workout ? (stage == 0 && !workoutInitialTargetSent ? 0 : trial.steps[qBound(0, stage, trial.steps.size() - 1)].incline) : (stage == 1 ? trial.incline : 0);
            if (pauseAt < 0 && (status.state != 3 || status.speedTenths != expectedSpeed || status.inclineCode != expectedIncline)) {
                stageReportedAt = -1;
                targetMatchCount = 0;
            }
            if (pauseAt < 0 && status.state == 3 && status.speedTenths == expectedSpeed && status.inclineCode == expectedIncline && stageReportedAt < 0) {
                ++targetMatchCount;
                if (!trial.workout || targetMatchCount >= 3) {
                    stageReportedAt = clock.elapsed();
                    record("reported_target", {{"stage", stage}, {"speed_code", expectedSpeed}, {"incline_code", expectedIncline},
                                               {"physical_attainment_verified", false},
                                               {"stage_name", trial.workout ? trial.steps[stage].name : QString()}});
                    if (trial.operatorMarks && stage > 0 && operatorArrivalAt[stage] < 0)
                        record("incline_state", {{"state", "target_reported"}, {"stage", stage}, {"physical_arrival_known", operatorArrivalAt[stage] >= 0}});
                }
            }
        }
        if (stopping && stopWritten) {
            postStopNormals = status.state == 0 ? postStopNormals + 1 : 0;
            if (postStopNormals >= 3) { feedbackAfterStop = true; close(); }
        }
    };
    QObject::connect(&control, &FitShow::Control::statusChanged, &app, observeStatus);
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &app, [&]() {
        buffer += process.readAllStandardOutput();
        if (buffer.size() > 1024 * 1024) { failed = true; close(); buffer.clear(); return; }
        int end;
        while ((end = buffer.indexOf('\n')) >= 0) {
            const auto line = buffer.left(end).trimmed(); buffer.remove(0, end + 1);
            if (line.startsWith('{')) {
                QJsonParseError error;
                const auto report = QJsonDocument::fromJson(line, &error).object();
                hasFinalReport = error.error == QJsonParseError::NoError && report.value("command") == "control-test" &&
                                 report.value("backend") == (linuxBackend ? "linux-bluez" : "windows-winrt") && report.value("motor_execution_enabled") == true;
                record("windows_report", report);
                if (!hasFinalReport || report.value("status") != "complete" || report.value("notification_cleanup_status") != "Success") failed = true;
                if (parser.isSet("observe-ftms") && report.value("ftms_observation_status") != "complete") failed = true;
                continue;
            }
            const auto parts = line.split('\t');
            if (parts.size() == 4 && parts[0] == "IDENTITY" && parts[1] == "FITSHOW" && parts[2] == "FS-BT-C1" && parts[3] == "1.3.8") {
                identityObserved = true;
                record("module_identity", {{"manufacturer", "FITSHOW"}, {"model", "FS-BT-C1"}, {"software_revision", "1.3.8"}, {"firmware", QJsonValue::Null}});
            } else if (parts.size() == 2 && parts[0] == "READY" && QString::fromUtf8(parts[1]) == name && identityObserved && !ready && !closing) {
                ready = true; control.setTransport(true); control.allowDiagnostics(true); control.queryLimits(10);
                record("transport_ready", {{"name", name}});
            } else if (parts.size() == 3 && parts[0] == "FTMS" && parser.isSet("observe-ftms") &&
                       (parts[1] == "00002acd-0000-1000-8000-00805f9b34fb" || parts[1] == "00002ada-0000-1000-8000-00805f9b34fb")) {
                record("ftms_notification", {{"characteristic_uuid", QString::fromLatin1(parts[1])},
                                              {"frame_hex", QString::fromLatin1(parts[2])}, {"physical_attainment_verified", false}});
            } else if (parts.size() == 2 && parts[0] == "RX") {
                const auto bytes = QByteArray::fromHex(parts[1]);
                record("notification", {{"frame_hex", QString::fromLatin1(parts[1])}});
                if (!closing) {
                    const auto response = FitShow::decodeResponse(bytes);
                    // A distinct bounded wake experiment admits documented sleep only
                    // during preflight and its two-second response window. Generic control still faults on every disabled state.
                    if (trial.sleepReadyAllowed && (!started || (!stopping && stageReportedAt < 0 && clock.elapsed() - startedAt < 2000)) &&
                        response.valid && response.command == 0x51 &&
                        response.status.state == 6 && response.status.disabledReason == 2)
                        observeStatus(response.status);
                    else if (waitWakeMs > 0 && !started && response.valid && response.command == 0x51 &&
                             response.status.state == 6 && response.status.disabledReason == 2) {
                        // Documented sleep before the start is the expected wait state. It never counts as NORMAL,
                        // and the key-detached state (06/01) still goes to the generic fault path.
                        normalCount = 0;
                        if (startPrompted || graceUntil >= 0 || startConfirmed) {
                            startPrompted = false; startConfirmed = false; graceUntil = -1;
                            record("wake_lost", {{"status_code", 6}});
                        }
                        if (!sleepNoticed) {
                            sleepNoticed = true;
                            record("waiting_for_wake", {{"wait_limit_seconds", waitWakeMs / 1000.0}});
                            QTextStream(stderr) << "Treadmill is asleep. Wake it at the console; nothing is sent to the motor meanwhile.\n" << Qt::flush;
                        } else if (clock.elapsed() - lastWaitNoticeAt >= 30000) {
                            QTextStream(stderr) << "Still waiting for the treadmill to wake (" << clock.elapsed() / 1000 << " s)...\n" << Qt::flush;
                        }
                        if (clock.elapsed() - lastWaitNoticeAt >= 30000) lastWaitNoticeAt = clock.elapsed();
                    }
                    else control.receive(bytes);
                }
            } else if (parts.size() == 4 && parts[0] == "TX") {
                record("write_completed", {{"command_id", QString::fromLatin1(parts[1])}, {"frame_hex", QString::fromLatin1(parts[2])},
                                           {"write_status", QString::fromLatin1(parts[3])}});
                if (parts[2] == "0253035003" && parts[3] == "Success") stopWritten = true;
                if (parts[3] != "Success") { failed = true; close(); }
            } else { failed = true; record("fault", {{"reason", "Invalid Windows bridge event"}}); close(); }
        }
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, &app, [&]() {
        QTextStream(stderr) << QString::fromUtf8(process.readAllStandardError());
    });
    QObject::connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &app,
                     [&](int code, QProcess::ExitStatus status) { if (code != 0 || status != QProcess::NormalExit) failed = true; app.quit(); });
    QObject::connect(&process, &QProcess::errorOccurred, &app, [&](QProcess::ProcessError error) {
        failed = true;
        if (error == QProcess::FailedToStart) {
            record("fault", {{"reason", linuxBackend ? "Linux Bluetooth helper failed to start" : "PowerShell failed to start; check WSL interop (powershell.exe from WSL)"}});
            QTextStream(stderr) << (linuxBackend ? "The Linux Bluetooth helper failed to start.\n"
                : "powershell.exe failed to start. If it reports 'Exec format error', WSL interop was unregistered; see README.\n");
            app.quit();
        }
    });
    QSocketNotifier signalNotifier(input.signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32]; const auto ignored = read(input.signalPipe[0], bytes, sizeof(bytes)); (void)ignored;
        failed = true; stop("Termination signal");
    });
    QSocketNotifier terminal(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&terminal, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32]; const auto count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count <= 0) { terminal.setEnabled(false); failed = true; stop("Operator input closed"); return; }
        for (ssize_t i = 0; i < count; ++i) {
            const char key = bytes[i];
            if ((key == '\n' || key == '\r') && startPrompted && !startConfirmed && !started) {
                startConfirmed = true;
                record("start_confirmed", {});
                continue;
            }
            if (key == 'q' || key == 'Q' || key == 's' || key == 'S') {
                failed = true; stop("Operator stop"); break;
            }
            if (!trial.operatorMarks || stage == 0 || stopping || closing) continue;
            if ((key == 'b' || key == 'B') && operatorBeginAt[stage] < 0 && operatorArrivalAt[stage] < 0) {
                operatorBeginAt[stage] = clock.elapsed();
                record("incline_state", {{"state", "moving_observed"}, {"stage", stage}, {"source", "operator_key"},
                                         {"command_to_motion_start_ms", operatorBeginAt[stage] - stageRequestedAt}});
            }
            const bool arrival = (stage == 1 && (key == 'u' || key == 'U')) || (stage == 2 && (key == 'd' || key == 'D'));
            if (arrival && operatorArrivalAt[stage] < 0 && operatorBeginAt[stage] >= 0) {
                operatorArrivalAt[stage] = clock.elapsed();
                record("incline_state", {{"state", "attained_observed"}, {"stage", stage}, {"source", "operator_key"},
                                         {"command_to_arrival_ms", operatorArrivalAt[stage] - stageRequestedAt},
                                         {"motion_duration_ms", operatorBeginAt[stage] < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(operatorArrivalAt[stage] - operatorBeginAt[stage])},
                                         {"report_preceded_arrival_ms", stageReportedAt < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(operatorArrivalAt[stage] - stageReportedAt)}});
                QTextStream(stderr) << "Physical arrival marked for stage " << stage << "; observing " << trial.arrivalHoldSeconds << " more seconds.\n" << Qt::flush;
            }
            else if (arrival && operatorBeginAt[stage] < 0)
                QTextStream(stderr) << "Arrival not recorded: press B when physical movement begins first.\n" << Qt::flush;
        }
    });
    QTimer pump;
    QObject::connect(&pump, &QTimer::timeout, &app, [&]() {
        const auto now = clock.elapsed();
        if (closing) return;
        if (!started && now > 30000 + waitWakeMs) { failed = true; record("fault", {{"reason", waitWakeMs > 0 ? "Treadmill did not wake within the wait limit" : "Preflight did not complete within 30 seconds"}}); close(); return; }
        if (started && !stopping && (now - latestStatusAt > 1500 || now - startedAt >= (trial.deadline - 2) * 1000 ||
            (stageReportedAt < 0 && pauseAt < 0 && now - stageRequestedAt > (stage == 0 ? 10000 : (trial.operatorMarks ? 60000 : 15000))))) {
            failed = true; stop("Missing status or start deadline");
        }
        if (trial.operatorMarks && stage > 0 && !stopping && operatorArrivalAt[stage] < 0 && now - stageRequestedAt >= 60000) {
            failed = true; stop("No physical incline arrival mark within 60 seconds");
        }
        if (pauseAt >= 0 && !stopping && now - pauseAt >= trial.stepHold * 1000) stop("Pause observation complete");
        if (stageReportedAt >= 0 && !stopping && pauseAt < 0) {
            const int hold = trial.workout ? trial.steps[stage].holdSeconds :
                (stage == 0 ? (trial.readyOnly ? trial.stepHold : (trial.id == "start-stop" ? 5 : 3)) : (stage == 1 ? trial.stepHold : trial.returnHold));
            const bool holdComplete = trial.operatorMarks && stage > 0
                ? operatorArrivalAt[stage] >= 0 && now - operatorArrivalAt[stage] >= trial.arrivalHoldSeconds * 1000
                : now - stageReportedAt >= hold * 1000;
            if (holdComplete) {
                if (trial.pause) {
                    pauseAt = now;
                    record("pause_requested", {{"physical_pause_verified", false}});
                    if (!closing) control.pause(1, 5);
                } else if (trial.workout && stage + 1 < trial.steps.size()) {
                    ++stage; stageReportedAt = -1; targetMatchCount = 0; stageRequestedAt = now;
                    const int speed = trial.steps[stage].speed, incline = trial.steps[stage].incline;
                    record("target_requested", {{"stage", stage}, {"speed_code", speed}, {"incline_code", incline}, {"stage_name", trial.steps[stage].name}});
                    if (!closing) control.target(1, stage + 1, speed, incline);
                } else if (trial.returnHold > 0 && stage < 2) {
                    ++stage; stageReportedAt = -1; stageRequestedAt = now;
                    const int speed = stage == 1 ? trial.speed : 10, incline = stage == 1 ? trial.incline : 0;
                    record("target_requested", {{"stage", stage}, {"speed_code", speed}, {"incline_code", incline}});
                    if (trial.operatorMarks) {
                        record("incline_state", {{"state", "target_requested"}, {"stage", stage}, {"target_code", incline}, {"physical_arrival_known", false}});
                        QTextStream(stderr) << "Incline " << incline << " requested. Press B when movement begins, then "
                                            << (stage == 1 ? 'U' : 'D') << " when it finishes.\n" << Qt::flush;
                    }
                    if (!closing) control.target(1, stage == 1 ? 2 : 4, speed, incline);
                } else stop("Prepared observation holds complete");
            }
        }
        if (stopping && now - stoppedAt >= 10000) {
            failed = true; record("fault", {{"reason", "Stop observation deadline; three NORMAL reports were not received"}}); close();
        }
        if (!closing) control.pump(now);
    });
    QTimer::singleShot(0, &app, [&]() {
        record("trial_plan", plan);
        if (closing) { app.quit(); return; }
        if (linuxBackend) {
            QStringList arguments {"--device", address.toString(), "--expected-name", name, "--confirm-motor",
                                   "--trial", trial.id, "--timeout", QString::number(trial.deadline)};
            if (trial.workout) arguments << "--target-sequence" << targetSequence;
            if (waitWakeMs > 0) arguments << "--wait-wake" << QString::number(waitWakeMs / 1000);
            process.start(helper, arguments);
        } else {
        QStringList arguments {"-Mta", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", helper,
                       "-Command", "control-test", "-Device", address.toString(), "-AddressType", parser.value("address-type"),
                       "-ExpectedName", name, "-Bridge", "-ConfirmMotor", "-Confirmation", "MOTOR", "-Trial", trial.id,
                       "-Timeout", QString::number(trial.deadline)};
        if (trial.workout) arguments << "-TargetSequence" << targetSequence;
        if (parser.isSet("observe-ftms")) arguments << "-ObserveFtms";
        process.start(qEnvironmentVariable("TREADMILL_POWERSHELL", "powershell.exe"), arguments);
        }
        pump.start(20);
    });
    app.exec();
    const bool sequenceObserved = started && stopWritten && feedbackAfterStop && hasFinalReport && !failed;
    record("trial_result", {{"sequence_observed", sequenceObserved}, {"stop_write_succeeded", stopWritten},
                            {"normal_status_after_stop", feedbackAfterStop}, {"physical_stop_verified", false},
                            {"operator_arrival_marks_recorded", operatorArrivalAt[1] >= 0 && operatorArrivalAt[2] >= 0},
                            {"ready_status_observed", trial.readyOnly && stageReportedAt >= 0}});
    return sequenceObserved && !failed ? 0 : 1;
}

int runWindowsWorkout(QCoreApplication &app, const QCommandLineParser &parser, const Treadmill::Workout &workout) {
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    Trial plan;
    plan.id = "workout";
    plan.workout = true;
    plan.workoutId = workout.id;
    plan.workoutName = workout.name;
    plan.speed = 10;
    plan.incline = 0;
    const bool linuxBackend = parser.value("backend") == "linux";
    // The Linux helper allows 120 minutes; the Windows bridge's gates were sized for 70 and stay there.
    const int maxMinutes = linuxBackend ? 120 : 70, maxDeadlineSeconds = linuxBackend ? 9000 : 4500;
    const qint64 totalMs = workout.durationMs();
    if (totalMs <= 0 || totalMs > qint64(maxMinutes) * 60 * 1000)
        return fail(QString("W1 hardware workouts on the %1 backend are limited to %2 minutes").arg(linuxBackend ? "Linux" : "Windows").arg(maxMinutes));
    constexpr double gradePerLevel = 11.0 / 8.0;
    for (const auto &stage : workout.stages) {
        for (qint64 offset = 0; offset < stage.durationMs;) {
            const qint64 span = qMin<qint64>(10000, stage.durationMs - offset);
            const auto target = stage.targetAt(offset + span);
            if (!std::isfinite(target.speedKmh) || !std::isfinite(target.gradePercent))
                return fail("Workout contains a non-finite W1 target: " + stage.name);
            const int speedCode = int(std::lround(target.speedKmh * 10.0));
            const int inclineCode = int(std::lround(target.gradePercent / gradePerLevel));
            if (speedCode < 10 || speedCode > 60 || inclineCode < 0 || inclineCode > 8 ||
                std::abs(target.speedKmh - speedCode / 10.0) > 0.051 ||
                std::abs(target.gradePercent - inclineCode * gradePerLevel) > gradePerLevel / 2.0 + 1e-8)
                return fail("Workout target is outside W1's confirmed 1.0–6.0 km/h and assumed 0–8 incline mapping: " + stage.name);
            const int hold = int((span + 999) / 1000);
            if (!plan.steps.isEmpty() && plan.steps.last().speed == speedCode && plan.steps.last().incline == inclineCode) {
                plan.steps.last().holdSeconds += hold;
            } else {
                plan.steps.append({speedCode, inclineCode, hold, stage.name});
                plan.speed = qMax(plan.speed, speedCode);
                plan.incline = qMax(plan.incline, inclineCode);
            }
            offset += span;
            if (plan.steps.size() > 512) return fail("Workout needs more than 512 distinct W1 targets");
        }
    }
    if (plan.steps.isEmpty()) return fail("Workout has no W1-compatible targets");
    // Each target costs about 1.5 s of waiting for three matching status reports (measured: 1.45 s mean over a
    // 60-minute run), so the margin grows with the step count: 3 s per step on top of the fixed 120 s.
    plan.deadline = int(totalMs / 1000) + 120 + 3 * int(plan.steps.size());
    if (plan.deadline > maxDeadlineSeconds) return fail(QString("Workout plus safety margin exceeds the %1 s control deadline").arg(maxDeadlineSeconds));
    plan.stepHold = plan.steps.first().holdSeconds;
    if (parser.value("backend") != "windows" && parser.value("backend") != "linux")
        return fail("Real W1 workout playback requires --backend windows or --backend linux");
    if (parser.value("device").toUpper() != "DD:87:70:39:74:20" || parser.value("expected-name") != "FS-0F7A32")
        return fail("Experimental W1 workout playback is pinned to DD:87:70:39:74:20 / FS-0F7A32");
    return runMotorTrial(app, parser, {}, false, {}, &plan);
}

int motorTrial(QCoreApplication &app, const QCommandLineParser &parser) {
    return runMotorTrial(app, parser, {}, false, {});
}

int motorBatch(QCoreApplication &app, const QCommandLineParser &parser) {
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    for (const QString option : {"trial", "list-trials", "confirm-motor", "simulate", "fast", "profile", "timeout", "confirm-diagnostics", "workouts-dir", "inject", "inject-at", "wait-wake", "wait-wake-confirm"})
        if (parser.isSet(option)) return fail("Unsupported batch option: " + option);
    const QString selection = parser.value("batch");
    if (selection != "remaining" && selection != "speeds" && selection != "inclines" && selection != "all")
        return fail("--batch must be remaining, speeds, inclines or all");
    if (parser.value("backend") != "windows" || QBluetoothAddress(parser.value("device")).isNull() || parser.value("expected-name").trimmed().isEmpty())
        return fail("Batch requires --backend windows, --device and exact --expected-name");
    QVector<Trial> selected;
    QJsonArray overview;
    for (const auto &trial : preparedTrials()) {
        bool include = selection == "all" || (selection == "speeds" && trial.id.startsWith("speed-")) ||
                       (selection == "inclines" && trial.id.startsWith("incline-")) ||
                       (selection == "remaining" && (trial.id == "speed-50" || trial.id == "speed-60" || trial.incline > 0 || trial.pause));
        if (!include || trial.operatorMarks || trial.readyOnly) continue; // Never add a human-timed experiment to an automatic batch.
        selected.append(trial);
        overview.append(QJsonObject {{"trial", trial.id}, {"speed_ceiling_code", trial.speed}, {"incline_ceiling_code", trial.incline},
                                     {"step_hold_seconds", trial.stepHold}, {"return_hold_seconds", trial.returnHold},
                                     {"windows_motor_deadline_seconds", trial.deadline}});
    }
    const QJsonObject plan {{"batch", selection}, {"trials", overview}, {"stop_between_trials", true},
                           {"abort_on_failure", true}, {"physical_stop_verified", false}};
    if (parser.isSet("dry-run")) {
        if (parser.isSet("confirm-batch") || parser.isSet("log")) return fail("Dry batch does not accept --confirm-batch or --log");
        QTextStream(stdout) << QJsonDocument(plan).toJson();
        return 0;
    }
    if (!parser.isSet("confirm-batch") || !parser.isSet("log") || !isatty(STDIN_FILENO))
        return fail("Batch requires --confirm-batch, --log <directory> and an interactive terminal");
    QDir directory(parser.value("log"));
    if (QFileInfo(directory.path()).exists() && !QFileInfo(directory.path()).isDir()) return fail("Batch log path must be a directory");
    for (const auto &trial : selected)
        if (QFileInfo::exists(directory.filePath(trial.id + ".jsonl"))) return fail("Batch log already exists: " + trial.id);
    QTextStream(stderr) << QJsonDocument(plan).toJson()
        << "Run this exact batch sequentially, with explicit stop and three NORMAL reports between trials.\n"
           "Keep the belt clear, safety key attached and physical stop reachable; remain at the controls.\n"
           "Raw incline is not percent grade. Any fault, Q/S or signal ends the batch.\n"
           "Type BATCH to approve all listed trials without individual prompts: " << Qt::flush;
    if (QTextStream(stdin).readLine() != "BATCH") return fail("Batch was not confirmed");
    if (!directory.exists() && !directory.mkpath(".")) return fail("Cannot create batch log directory");
    int completed = 0;
    for (const auto &trial : selected) {
        QTextStream(stderr) << "Batch trial " << ++completed << '/' << selected.size() << ": " << trial.id << '\n' << Qt::flush;
        const int result = runMotorTrial(app, parser, trial.id, true, directory.filePath(trial.id + ".jsonl"));
        if (result != 0) {
            QTextStream(stderr) << "Batch halted at " << trial.id << "; no further trial will start.\n";
            return result;
        }
    }
    QTextStream(stderr) << "Batch completed " << completed << " trials; physical observations require operator review.\n";
    return 0;
}
