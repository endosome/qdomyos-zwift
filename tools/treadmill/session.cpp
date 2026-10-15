#include "session.h"
#include "runner.h"
#include "profile.h"
#include "terminal.h"
#include "motortrial.h"
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUuid>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
using namespace Treadmill;

void addWorkoutOptions(QCommandLineParser &parser) {
    parser.addOption({"confirm-diagnostics", "Enable the interactive nonmotor status confirmation prompt."});
    parser.addOption({"profile", "Device profile for offline mapping preview (never enables motor execution).", "file"});
    parser.addOption({"simulate", "Use the simulator; no Bluetooth objects are created."});
    parser.addOption({"fast", "Advance simulation deterministically without waiting (no terminal controls)."});
    parser.addOption({"workouts-dir", "Directory of external XML workout plans.", "directory"});
    parser.addOption({"log", "Session JSON Lines destination (must not already exist).", "file"});
    parser.addOption({"inject", "Simulation fault: stale, disconnect, override, stop-failure, reject, no-ack.", "fault"});
    parser.addOption({"inject-at", "Active workout seconds before injecting fault.", "seconds", "5"});
}
static QJsonObject pairJson(Target target) {
    return {{"speed_kmh", target.speedKmh}, {"grade_percent", target.gradePercent}};
}
static QString workoutDirectory(const QCommandLineParser &parser) {
    if (parser.isSet("workouts-dir")) return parser.value("workouts-dir");
    const QString installed = QCoreApplication::applicationDirPath() + "/../share/treadmill/workouts";
    if (QDir(installed).exists()) return installed;
    return QStringLiteral(TREADMILL_WORKOUT_DIR);
}
static bool catalog(const QString &directory, QMap<QString, QString> &paths, QJsonArray &entries, QString &error) {
    const QDir dir(directory);
    if (!dir.exists()) { error = "Workout directory does not exist: " + directory; return false; }
    for (const auto &file : dir.entryList({"*.xml"}, QDir::Files, QDir::Name)) {
        const QString path = dir.absoluteFilePath(file);
        const auto loaded = loadWorkout(path);
        if (!loaded.error.isEmpty()) { error = path + ": " + loaded.error; return false; }
        const auto &workout = loaded.workout;
        if (paths.contains(workout.id)) { error = "Duplicate workout ID: " + workout.id; return false; }
        paths.insert(workout.id, path);
        entries.append(QJsonObject{{"id", workout.id}, {"name", workout.name},
                                   {"duration_seconds", double(workout.durationMs()) / 1000}, {"path", path}});
    }
    return true;
}


int workoutCommand(QCoreApplication &app, const QCommandLineParser &parser, const QStringList &arguments) {
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    if (arguments.size() < 2 || (arguments[1] != "list" && arguments[1] != "run" && arguments[1] != "inspect"))
        return fail("Usage: treadmill workout list | run <id-or-path> --simulate [--fast] | run <id-or-path> --backend windows --device <address> --expected-name <name> [--dry-run | --confirm-motor --log <new-file>]");
    if (arguments[1] == "list") {
        if (arguments.size() != 2 || parser.isSet("simulate") || parser.isSet("fast") ||
            parser.isSet("log") || parser.isSet("inject") || parser.isSet("inject-at") || parser.isSet("profile") ||
            parser.isSet("backend") || parser.isSet("device") || parser.isSet("expected-name") || parser.isSet("confirm-motor") || parser.isSet("dry-run")) return fail("Invalid workout list options");
        QMap<QString, QString> paths;
        QJsonArray entries;
        QString error;
        if (!catalog(workoutDirectory(parser), paths, entries, error)) return fail(error);
        QTextStream(stdout) << QJsonDocument(entries).toJson(QJsonDocument::Indented);
        return 0;
    }
    if (arguments.size() != 3) return fail("workout run requires one workout ID or file path");
    const bool inspect = arguments[1] == "inspect";
    if (!inspect && !parser.isSet("simulate")) {
        if (!parser.isSet("backend") || parser.value("backend") != "windows" || !parser.isSet("device") ||
            !parser.isSet("expected-name"))
            return fail("Hardware workout requires --backend windows, --device and exact --expected-name");
        if (parser.isSet("profile") || parser.isSet("fast") || parser.isSet("inject") || parser.isSet("inject-at") ||
            parser.isSet("confirm-diagnostics")) return fail("Unsupported hardware workout option");
    }
    if (!inspect && parser.isSet("simulate") && (parser.isSet("profile") || parser.isSet("backend") || parser.isSet("device") || parser.isSet("expected-name") || parser.isSet("confirm-motor") || parser.isSet("dry-run")))
        return fail("Hardware options cannot be used with simulation");
    if (inspect && (!parser.isSet("profile") || parser.isSet("simulate") || parser.isSet("fast") || parser.isSet("log") ||
                    parser.isSet("inject") || parser.isSet("inject-at") || parser.isSet("backend") || parser.isSet("device") ||
                    parser.isSet("expected-name") || parser.isSet("confirm-motor") || parser.isSet("dry-run"))) return fail("workout inspect requires --profile and no execution options");
    QString path = arguments[2];
    if (!QFileInfo::exists(path)) {
        QMap<QString, QString> paths;
        QJsonArray entries;
        QString error;
        if (!catalog(workoutDirectory(parser), paths, entries, error)) return fail(error);
        if (!paths.contains(path)) return fail("Unknown workout ID: " + path);
        path = paths.value(path);
    }
    const auto loaded = loadWorkout(path);
    if (!loaded.error.isEmpty()) return fail(loaded.error);
    if (!inspect && !parser.isSet("simulate")) return runWindowsWorkout(app, parser, loaded.workout);
    if (inspect) {
        const auto profile = loadProfile(parser.value("profile"));
        if (!profile.error.isEmpty()) return fail(profile.error);
        auto geometry = profile.profile.capabilities();
        geometry.validated = true; // Offline feasibility preview, never passed to a controller.
        const auto error = validateWorkout(loaded.workout, geometry);
        if (!error.isEmpty()) return fail(error);
        QJsonArray stages;
        for (const auto &stage : loaded.workout.stages) {
            Target from, to;
            if (!profile.profile.map(stage.from, from) || !profile.profile.map(stage.to, to)) return fail("Unmappable stage: " + stage.name);
            stages.append(QJsonObject{{"name", stage.name}, {"duration_seconds", double(stage.durationMs) / 1000},
                                     {"requested_from", pairJson(stage.from)}, {"requested_to", pairJson(stage.to)},
                                     {"mapped_from", pairJson(from)}, {"mapped_to", pairJson(to)}});
        }
        QJsonObject report{{"workout_id", loaded.workout.id}, {"preview_only", true}, {"motor_execution_enabled", false},
                           {"profile_motor_evidence_complete", profile.profile.motorEvidenceComplete()}, {"stages", stages}};
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
        return 0;
    }
    Simulator simulator;
    Runner runner(simulator, loaded.workout);
    const auto validation = validateWorkout(loaded.workout, simulator.capabilities());
    if (!validation.isEmpty()) return fail(validation);
    const QMap<QString, Simulator::Failure> failures = {
        {"stale", Simulator::Failure::StaleTelemetry}, {"disconnect", Simulator::Failure::Disconnect},
        {"override", Simulator::Failure::ManualOverride}, {"stop-failure", Simulator::Failure::StopFailure},
        {"reject", Simulator::Failure::RejectTarget}, {"no-ack", Simulator::Failure::NoAcknowledgements}};
    if (parser.isSet("inject") && !failures.contains(parser.value("inject"))) return fail("Unknown simulation fault");
    bool validInjectionTime = false;
    const qint64 injectSeconds = parser.value("inject-at").toLongLong(&validInjectionTime);
    if (!validInjectionTime || injectSeconds < 0 || injectSeconds > loaded.workout.durationMs() / 1000)
        return fail("--inject-at must be an integer within the workout duration");
    if (parser.isSet("inject-at") && !parser.isSet("inject")) return fail("--inject-at requires --inject");

    const QString sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString logPath = parser.value("log");
    if (logPath.isEmpty()) {
        const QString logDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sessions";
        if (!QDir().mkpath(logDirectory)) return fail("Cannot create session log directory");
        logPath = logDirectory + "/" + sessionId + ".jsonl";
    }
    QFile log(logPath);
    if (!log.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail("Cannot create session log: " + log.errorString());
    bool logFailed = false;
    qint64 nowMs = 0;
    auto record = [&](const QString &type, const QString &message, QJsonObject fields = {}) {
        if (logFailed) return;
        const auto sample = runner.telemetry();
        fields.insert("session_id", sessionId);
        fields.insert("generation", QString::number(runner.generation()));
        fields.insert("utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        fields.insert("monotonic_ms", nowMs);
        fields.insert("elapsed_ms", runner.elapsedMs());
        fields.insert("event", type);
        fields.insert("message", message);
        fields.insert("state", Runner::stateName(runner.state()));
        fields.insert("stage_index", runner.stageIndex());
        fields.insert("requested", pairJson(runner.requested()));
        fields.insert("reported", pairJson(sample.actual));
        fields.insert("telemetry_age_ms", sample.atMs < 0 ? -1 : nowMs - sample.atMs);
        fields.insert("connected", sample.connected);
        fields.insert("running", sample.running);
        fields.insert("paused", sample.paused);
        const auto line = QJsonDocument(fields).toJson(QJsonDocument::Compact) + '\n';
        if (log.size() + line.size() > 32 * 1024 * 1024 || log.write(line) != line.size()) {
            logFailed = true;
            QTextStream(stderr) << "Session log write/size failure; aborting workout.\n";
            runner.abort("Session recording failed");
        }
    };
    QObject::connect(&runner, &Runner::logEvent, &app, [&](const QString &type, const QString &message) {
        if (type != "command") record(type, message);
        if (type == "stage" || type == "notification" || type == "instruction" || type == "user")
            QTextStream(stdout) << type << ": " << message << '\n';
    });
    QObject::connect(&simulator, &Controller::commandEvent, &app, [&](quint64 id, const QString &phase) {
        record("command", phase, {{"command_id", QString::number(id)}, {"phase", phase}});
    });
    QObject::connect(&runner, &Runner::commandTimedOut, &app, [&](quint64 id) {
        record("command", "acknowledgement_timeout", {{"command_id", QString::number(id)}, {"phase", "acknowledgement_timeout"}});
    });
    QObject::connect(&runner, &Runner::commandSettled, &app, [&](quint64 id) {
        record("command", "settled", {{"command_id", QString::number(id)}, {"phase", "settled"}});
    });
    QObject::connect(&runner, &Runner::stateChanged, &app, [&](const QString &state) {
        record("state", state);
        QTextStream(stdout) << "State: " << state << '\n';
    });
    record("session_start", loaded.workout.id, {{"workout_id", loaded.workout.id}, {"backend", "simulator"}});
    QTextStream(stdout) << "SIMULATION: " << loaded.workout.name << " (" << loaded.workout.durationMs() / 1000
                        << " seconds)\nLog: " << logPath << '\n';
    InputGuard input(!parser.isSet("fast"));
    if (!input.valid()) return fail("Cannot install signal handling");
    QSocketNotifier signalNotifier(input.signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32];
        const auto ignored = ::read(input.signalPipe[0], bytes, sizeof(bytes));
        (void)ignored;
        record("user", "Termination signal");
        runner.abort("Termination signal");
    });
    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    stdinNotifier.setEnabled(!parser.isSet("fast"));
    QObject::connect(&stdinNotifier, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32];
        const auto count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count == 0) {
            stdinNotifier.setEnabled(false);
            record("user", "Standard input closed");
            runner.abort("Standard input closed");
            return;
        }
        if (count < 0) {
            if (errno != EINTR && errno != EAGAIN) runner.abort("Terminal input failed");
            return;
        }
        for (ssize_t i = 0; i < count; ++i) {
            switch (bytes[i]) {
            case 's': case 'S': case 'q': case 'Q': record("user", "Stop/quit"); runner.abort("User stop"); break;
            case 'p': case 'P': record("user", "Pause"); runner.pause(); break;
            case 'r': case 'R': record("user", "Resume confirmation"); runner.resume(); break;
            case '-': record("user", "Reduce intensity"); runner.reduce(); break;
            default: break;
            }
        }
    });
    if (!parser.isSet("fast")) QTextStream(stdout) << "[P] Pause [R] Resume [S/Q] Stop/quit [-] Reduce\n";
    runner.prepare(0);
    bool injected = false;
    qint64 lastDisplayMs = -1000;
    qint64 lastFlushMs = 0;
    auto tick = [&]() {
        if (parser.isSet("inject") && !injected && runner.state() == Runner::State::Running &&
            runner.elapsedMs() >= injectSeconds * 1000) {
            simulator.inject(failures.value(parser.value("inject")));
            injected = true;
            record("injection", parser.value("inject"));
        }
        runner.advance(nowMs);
        if (runner.state() == Runner::State::Ready) runner.confirmStart(); // Simulation only.
        if (!parser.isSet("fast") && nowMs - lastDisplayMs >= 1000) {
            lastDisplayMs = nowMs;
            const auto sample = runner.telemetry();
            const auto target = runner.requested();
            const auto &stage = loaded.workout.stages[runner.stageIndex()];
            QTextStream(stdout) << runner.elapsedMs() / 1000 << '/' << loaded.workout.durationMs() / 1000
                << "s " << stage.name << " actual=" << sample.actual.speedKmh << "km/h " << sample.actual.gradePercent
                << "% target=" << target.speedKmh << "km/h " << target.gradePercent << "% age=" << nowMs - sample.atMs << "ms\n";
        }
        if (nowMs - lastFlushMs >= 1000) {
            lastFlushMs = nowMs;
            if (!log.flush() && !logFailed) {
                logFailed = true;
                QTextStream(stderr) << "Session log flush failed; aborting.\n";
                runner.abort("Session log flush failed");
            }
        }
    };
    if (parser.isSet("fast")) {
        while (!runner.finished() && nowMs < loaded.workout.durationMs() + 3600000) {
            nowMs += 200;
            tick();
            QCoreApplication::processEvents();
        }
        if (!runner.finished()) {
            runner.abort("Simulation exceeded time budget");
            for (int i = 0; i < 60 && !runner.finished(); ++i) { nowMs += 200; tick(); }
        }
    } else {
        QElapsedTimer clock;
        clock.start();
        QTimer timer;
        timer.setInterval(100);
        QObject::connect(&timer, &QTimer::timeout, &app, [&]() {
            nowMs = clock.elapsed();
            tick();
            if (runner.finished()) app.quit();
        });
        timer.start();
        if (!runner.finished()) app.exec();
    }
    simulator.disconnectDevice();
    record("session_end", runner.reason(), {{"workout_id", loaded.workout.id}, {"backend", "simulator"}});
    const bool flushed = log.flush();
    QTextStream(stdout) << Runner::stateName(runner.state()) << ": " << runner.reason() << '\n';
    return runner.state() == Runner::State::Completed && !logFailed && flushed ? 0 : 1;
}

int simulatorStatus() {
    Simulator simulator;
    Telemetry telemetry;
    QObject::connect(&simulator, &Controller::sample, [&](Telemetry sample) { telemetry = sample; });
    simulator.connectDevice(1);
    simulator.advance(200);
    simulator.advance(400);
    const auto capabilities = simulator.capabilities();
    QJsonObject limits{{"speed_unit", "km/h"}, {"incline_unit", "percent"},
                       {"min_speed", capabilities.minSpeedKmh}, {"max_speed", capabilities.maxSpeedKmh},
                       {"speed_resolution", capabilities.speedStepKmh},
                       {"min_incline", capabilities.minGradePercent}, {"max_incline", capabilities.maxGradePercent},
                       {"incline_resolution", capabilities.gradeStepPercent}};
    QJsonObject report{{"backend", "simulator"}, {"connected", telemetry.connected},
                       {"running", telemetry.running}, {"reported", pairJson(telemetry.actual)},
                       {"simulation_limits", limits}};
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
    return 0;
}
