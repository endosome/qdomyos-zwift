#include "diagnostics.h"
#include "session.h"
#include <QBluetoothAddress>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("treadmill"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Read-only Linux BLE treadmill diagnostics."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("scan, info, workout list, or workout run <id-or-path>."));
    QCommandLineOption timeoutOption(QStringLiteral("timeout"), QStringLiteral("Scan duration (1–60 seconds)."),
                                    QStringLiteral("seconds"), QStringLiteral("10"));
    QCommandLineOption deviceOption(QStringLiteral("device"), QStringLiteral("Explicit Bluetooth address for info."),
                                   QStringLiteral("address"));
    parser.addOption(timeoutOption);
    parser.addOption(deviceOption);
    addWorkoutOptions(parser);
    parser.process(app);
    const auto arguments = parser.positionalArguments();
    if (!arguments.isEmpty() && arguments.first() == "workout") {
        if (parser.isSet(deviceOption) || parser.isSet(timeoutOption)) {
            QTextStream(stderr) << "--device/--timeout apply to BLE diagnostics.\n";
            return 2;
        }
        return workoutCommand(app, parser, arguments);
    }
    for (const QString option : {"simulate", "fast", "workouts-dir", "log", "inject", "inject-at"}) {
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
        if (parser.isSet(deviceOption)) {
            QTextStream(stderr) << "--device applies to info.\n";
            return 2;
        }
        return scanDevices(app, timeout);
    }
    const QBluetoothAddress address(parser.value(deviceOption));
    if (!parser.isSet(deviceOption) || address.isNull()) {
        QTextStream(stderr) << "info requires --device with a valid Bluetooth address.\n";
        return 2;
    }
    return inspectDevice(app, address, timeout);
}
