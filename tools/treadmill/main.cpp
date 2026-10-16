#include "diagnostics.h"
#include "session.h"
#include "probe.h"
#include "windowsbackend.h"
#include "motortrial.h"
#include <QBluetoothAddress>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("treadmill"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Linux/Windows BLE diagnostics, bounded Windows control trials and W1 workouts."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("scan, info, status, control-test, workout list, or workout run <id-or-path>."));
    QCommandLineOption timeoutOption(QStringLiteral("timeout"), QStringLiteral("Diagnostic duration: 1–60 seconds; Windows status supports 1–600, or 900 with --measure-sleep-timer."),
                                    QStringLiteral("seconds"), QStringLiteral("10"));
    QCommandLineOption deviceOption(QStringLiteral("device"), QStringLiteral("Explicit Bluetooth address for info or Windows status/control-test."),
                                   QStringLiteral("address"));
    parser.addOption(timeoutOption);
    parser.addOption(deviceOption);
    parser.addOption(QCommandLineOption("backend", "BLE backend: linux (default) or windows via WSL.", "backend", "linux"));
    parser.addOption(QCommandLineOption("address-type", "Windows BLE address type: unknown, public or random.", "type", "unknown"));
    parser.addOption(QCommandLineOption("expected-name", "Exact confirmed name for Windows status/control-test with --device.", "name"));
    parser.addOption(QCommandLineOption("confirm-motor", "Approve the separately bounded motor control trial."));
    parser.addOption(QCommandLineOption("dry-run", "Print the bounded control trial without opening Bluetooth."));
    parser.addOption(QCommandLineOption("trial", "Prepared control trial ID; see --list-trials.", "id", "start-stop"));
    parser.addOption(QCommandLineOption("list-trials", "List all prepared control trials without Bluetooth I/O."));
    parser.addOption(QCommandLineOption("batch", "Prepared batch: remaining, speeds, inclines or all.", "set", "remaining"));
    parser.addOption(QCommandLineOption("confirm-batch", "Approve the printed sequential batch with stop between trials."));
    parser.addOption(QCommandLineOption("wait-wake", "Linux hardware runs: wait up to this many minutes (0.05-30) for the console to wake, sending nothing to the motor, then run the normal preflight. Without --wait-wake-confirm the belt starts by itself 5 s after the wake.", "minutes"));
    parser.addOption(QCommandLineOption("wait-wake-confirm", "With --wait-wake: after the wake, start only when ENTER is pressed."));
    parser.addOption(QCommandLineOption("running-mode", "Linux hardware runs: allow speeds above 6.0 km/h (the console must be in running mode). Needs FTMS observation, unlocks in stages (see the run-N trials) and stops if FTMS speed disagrees with FitShow speed."));
    parser.addOption(QCommandLineOption("observe-ftms", "Also capture FTMS data/status notifications without FTMS motor writes."));
    parser.addOption(QCommandLineOption("status-heart-rate-zero", "Experimental Windows status query with the documented zero heart-rate byte."));
    parser.addOption(QCommandLineOption("status-data-sport", "Experimental Windows status plus documented current-exercise-data query."));
    parser.addOption(QCommandLineOption("measure-sleep-timer", "Windows nonmotor observer: start asleep, wake physically, measure first NORMAL to next sleep."));
    addWorkoutOptions(parser);
    // `treadmill workout run <plan>` defaults to the RunPad W1 with a timestamped log; the typed
    // MOTOR confirmation still applies. Under WSL it uses the Windows bridge, elsewhere BlueZ.
    QStringList commandLine = app.arguments();
    if (commandLine.size() >= 4 && commandLine[1] == "workout" && commandLine[2] == "run" &&
        !commandLine.contains("--simulate") && !commandLine.contains("--backend")) {
        const bool underWsl = qEnvironmentVariableIsSet("WSL_DISTRO_NAME");
        commandLine << "--backend" << (underWsl ? "windows" : "linux") << "--device" << "DD:87:70:39:74:20"
                    << "--expected-name" << "FS-0F7A32";
        if (underWsl) commandLine << "--address-type" << "public";
        if (!commandLine.contains("--dry-run") && !commandLine.contains("--log")) {
            const QString logDirectory = QDir::homePath() + "/treadmill-logs";
            QDir().mkpath(logDirectory);
            commandLine << "--confirm-motor" << "--log"
                        << logDirectory + '/' + QFileInfo(commandLine[3]).completeBaseName() + '-' +
                               QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".jsonl";
        }
    }
    if (commandLine.contains("--running-mode") && !commandLine.contains("--observe-ftms")) commandLine << "--observe-ftms"; // required, not optional
    parser.process(commandLine);
    const auto arguments = parser.positionalArguments();
    const bool hardwareWorkout = arguments.size() == 3 && arguments[0] == "workout" && arguments[1] == "run" && !parser.isSet("simulate");
    const QString backend = parser.value("backend");
    if (backend != "linux" && backend != "windows") {
        QTextStream(stderr) << "--backend must be linux or windows.\n"; return 2;
    }
    const QString addressType = parser.value("address-type");
    if (addressType != "unknown" && addressType != "public" && addressType != "random") {
        QTextStream(stderr) << "--address-type must be unknown, public or random.\n"; return 2;
    }
    if (parser.isSet("address-type") && backend != "windows") {
        QTextStream(stderr) << "--address-type requires --backend windows.\n"; return 2;
    }
    if (parser.isSet("measure-sleep-timer") && (parser.isSet("status-data-sport") || parser.isSet("status-heart-rate-zero") ||
        backend != "windows" || arguments.size() != 1 || arguments.first() != "status" || parser.isSet("simulate"))) {
        QTextStream(stderr) << "--measure-sleep-timer requires Windows protocol status and excludes experimental query variants.\n"; return 2;
    }
    if (parser.isSet("status-data-sport") && (parser.isSet("status-heart-rate-zero") || backend != "windows" || arguments.size() != 1 ||
        arguments.first() != "status" || parser.isSet("simulate"))) {
        QTextStream(stderr) << "--status-data-sport requires Windows protocol status and excludes --status-heart-rate-zero.\n"; return 2;
    }
    if (parser.isSet("status-heart-rate-zero") && (backend != "windows" || arguments.size() != 1 ||
        arguments.first() != "status" || parser.isSet("simulate"))) {
        QTextStream(stderr) << "--status-heart-rate-zero requires Windows protocol status.\n"; return 2;
    }
    if (parser.isSet("observe-ftms") && ((backend != "windows" && backend != "linux") ||
        (backend == "linux" && !hardwareWorkout && (arguments.size() != 1 || (arguments.first() != "control-test" && arguments.first() != "control-batch"))) ||
        (!hardwareWorkout && (arguments.size() != 1 ||
         (arguments.first() != "status" && arguments.first() != "control-test" && arguments.first() != "control-batch"))) ||
        parser.isSet("simulate"))) {
        QTextStream(stderr) << "--observe-ftms requires control trials or hardware workouts (Windows also supports protocol status).\n"; return 2;
    }
    if ((parser.isSet("wait-wake") || parser.isSet("wait-wake-confirm")) &&
        !(hardwareWorkout || (arguments.size() == 1 && arguments.first() == "control-test")) ) {
        QTextStream(stderr) << "--wait-wake applies to hardware workouts and control-test only.\n"; return 2;
    }
    if (parser.isSet("running-mode") && (backend != "linux" || parser.isSet("simulate") ||
        !(hardwareWorkout || (arguments.size() == 1 && arguments.first() == "control-test")))) {
        QTextStream(stderr) << "--running-mode applies to Linux-backend control trials and hardware workouts only.\n"; return 2;
    }
    if (arguments.size() == 1 && arguments.first() == "control-batch") return motorBatch(app, parser);
    if (parser.isSet("batch") || parser.isSet("confirm-batch")) {
        QTextStream(stderr) << "Batch options apply only to control-batch.\n"; return 2;
    }
    if (arguments.size() == 1 && arguments.first() == "control-test") return motorTrial(app, parser);
    if ((parser.isSet("confirm-motor") || parser.isSet("dry-run")) && !hardwareWorkout) {
        QTextStream(stderr) << "Motor approval/dry-run options apply to control trials or hardware workouts.\n"; return 2;
    }
    if (parser.isSet("trial") || parser.isSet("list-trials")) {
        QTextStream(stderr) << "Control trial options apply only to control-test.\n"; return 2;
    }
    if ((parser.isSet("backend") || parser.isSet("address-type")) && parser.isSet("simulate")) {
        QTextStream(stderr) << "Backend options cannot be used with simulation.\n"; return 2;
    }
    if (parser.isSet("expected-name") && ((backend != "windows" && !hardwareWorkout) || parser.isSet("simulate") || !parser.isSet("device") ||
        (!hardwareWorkout && (arguments.size() != 1 || arguments.first() != "status")))) {
        QTextStream(stderr) << "--expected-name requires Windows status or hardware workout with --device.\n"; return 2;
    }
    if (!arguments.isEmpty() && arguments.first() == "status") {
        if (arguments.size() == 1 && !parser.isSet("simulate")) {
            if (backend != "windows" && parser.isSet("device")) {
                QTextStream(stderr) << "Linux protocol status uses --profile rather than --device.\n"; return 2;
            }
            for (const QString option : {"fast", "workouts-dir", "inject", "inject-at"}) {
                if (parser.isSet(option)) { QTextStream(stderr) << "Unsupported protocol status option: " << option << '\n'; return 2; }
            }
            return probeStatus(app, parser);
        }
        if (arguments.size() != 1 || !parser.isSet("simulate")) {
            QTextStream(stderr) << "Hardware telemetry remains unverified; use treadmill status --simulate.\n";
            return 2;
        }
        for (const QString option : {"device", "timeout", "fast", "workouts-dir", "log", "inject", "inject-at", "profile", "confirm-diagnostics"}) {
            if (parser.isSet(option)) {
                QTextStream(stderr) << "Unsupported status option: " << option << '\n';
                return 2;
            }
        }
        return simulatorStatus();
    }
    if (!arguments.isEmpty() && arguments.first() == "workout") {
        if (parser.isSet("confirm-diagnostics")) { QTextStream(stderr) << "--confirm-diagnostics applies to protocol status only.\n"; return 2; }
        if (parser.isSet(timeoutOption) || (parser.isSet(deviceOption) && !hardwareWorkout)) {
            QTextStream(stderr) << "--timeout applies to BLE diagnostics; --device is accepted only for a Windows hardware workout here.\n";
            return 2;
        }
        return workoutCommand(app, parser, arguments);
    }
    for (const QString option : {"simulate", "fast", "workouts-dir", "log", "inject", "inject-at", "profile", "confirm-diagnostics"}) {
        if (parser.isSet(option)) {
            QTextStream(stderr) << option << " applies to workout commands.\n";
            return 2;
        }
    }
    if (arguments.size() != 1 || (arguments.first() != "scan" && arguments.first() != "info")) {
        QTextStream(stderr) << "Usage: treadmill scan | info --device <address> [--timeout <seconds>]\n";
        return 2;
    }
    bool validTimeout = false;
    const int timeout = parser.value(timeoutOption).toInt(&validTimeout);
    if (!validTimeout || timeout < 1 || timeout > 60) {
        QTextStream(stderr) << "--timeout must be an integer between 1 and 60 seconds.\n";
        return 2;
    }
    if (arguments.first() == "scan") {
        if (parser.isSet(deviceOption) || parser.isSet("address-type")) {
            QTextStream(stderr) << "--device/--address-type apply to info or protocol status.\n";
            return 2;
        }
        if (backend == "windows") return windowsDiagnostics(app, "scan", timeout);
        return scanDevices(app, timeout);
    }
    const QBluetoothAddress address(parser.value(deviceOption));
    if (!parser.isSet(deviceOption) || address.isNull()) {
        QTextStream(stderr) << "info requires --device with a valid Bluetooth address.\n";
        return 2;
    }
    if (backend == "windows") return windowsDiagnostics(app, "info", timeout, address.toString(), addressType);
    return inspectDevice(app, address, timeout);
}
