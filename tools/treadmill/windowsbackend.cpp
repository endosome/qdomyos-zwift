#include "windowsbackend.h"
#include "terminal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <memory>

QString windowsHelperPath(QString &error) {
    QString helper = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        "../share/treadmill/windows/windows-diagnostics.ps1");
    if (!QFileInfo::exists(helper)) helper = QStringLiteral(TREADMILL_WINDOWS_HELPER);
    if (!QFileInfo::exists(helper)) { error = "Windows Bluetooth helper is missing: " + helper; return {}; }
    // Use argument arrays throughout; no shell evaluates addresses, names or paths.
    QProcess path;
    path.start("wslpath", {"-w", QFileInfo(helper).absoluteFilePath()});
    if (!path.waitForStarted(2000) || !path.waitForFinished(2000) || path.exitCode() != 0)
        { error = "Windows backend requires WSL interop and wslpath"; return {}; }
    const QString windowsPath = QString::fromUtf8(path.readAllStandardOutput()).trimmed();
    if (windowsPath.isEmpty()) error = "wslpath returned an empty helper path";
    return windowsPath;
}

int windowsDiagnostics(QCoreApplication &app, const QString &command, int seconds,
                       const QString &address, const QString &addressType,
                       const QString &expectedName, const QString &logPath, bool observeFtms, bool statusHeartRateZero, bool statusDataSport, bool measureSleepTimer) {
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 1; };
    QString error;
    const QString windowsPath = windowsHelperPath(error);
    if (!error.isEmpty()) return fail(error);
    QFile log(logPath);
    if (!logPath.isEmpty() && !log.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail("Cannot create diagnostic log: " + log.errorString());
    InputGuard input(command == "probe");
    if (!input.valid()) return fail("Cannot install Windows diagnostic signal handling");
    QProcess process;
    QStringList arguments = {"-Mta", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                             "-File", windowsPath, "-Command", command, "-Timeout", QString::number(seconds)};
    if (!address.isEmpty()) arguments << "-Device" << address << "-AddressType" << addressType;
    if (command == "probe") arguments << "-ExpectedName" << expectedName << "-ConfirmDiagnostics"
                                     << "-Confirmation" << "NONMOTOR" << "-Bridge";
    if (observeFtms) arguments << "-ObserveFtms";
    if (statusHeartRateZero) arguments << "-StatusHeartRateZero";
    if (statusDataSport) arguments << "-StatusDataSport";
    if (measureSleepTimer) arguments << "-MeasureSleepTimer";
    QByteArray output, errors;
    bool sleepReadyAnnounced = false;
    bool cancelled = false, failed = false;
    QTimer cleanup;
    cleanup.setSingleShot(true);
    auto cancel = [&]() {
        if (cancelled) return;
        cancelled = true;
        // The helper closes notifications/services before emitting its final report.
        if (command == "probe") { process.write("QUIT\n"); process.closeWriteChannel(); }
        else process.terminate();
        cleanup.start(observeFtms ? 10000 : 5000);
    };
    QObject::connect(&cleanup, &QTimer::timeout, &app, [&]() {
        failed = true;
        QTextStream(stderr) << "Windows helper did not exit; notification cleanup is unconfirmed.\n";
        process.kill();
    });
    auto collect = [&](bool standardOutput) {
        QByteArray &bytes = standardOutput ? output : errors;
        bytes += standardOutput ? process.readAllStandardOutput() : process.readAllStandardError();
        if (!standardOutput && measureSleepTimer && !sleepReadyAnnounced && errors.contains("SLEEP_TIMER_READY")) {
            sleepReadyAnnounced = true;
            QTextStream(stderr) << "Sleep-timer observer confirmed initial sleep. Wake the treadmill physically now, with belt stopped.\n" << Qt::flush;
        }
        if (bytes.size() > 8 * 1024 * 1024) { failed = true; bytes.truncate(8 * 1024 * 1024); cancel(); }
    };
    QObject::connect(&process, &QProcess::readyReadStandardOutput, &app, [&]() { collect(true); });
    QObject::connect(&process, &QProcess::readyReadStandardError, &app, [&]() { collect(false); });
    QObject::connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &app,
                     [&](int, QProcess::ExitStatus) { app.quit(); });
    QObject::connect(&process, &QProcess::errorOccurred, &app, [&](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { failed = true; app.quit(); }
    });
    QSocketNotifier signalNotifier(input.signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32]; const auto ignored = read(input.signalPipe[0], bytes, sizeof(bytes)); (void)ignored;
        cancel();
    });
    std::unique_ptr<QSocketNotifier> terminal;
    if (command == "probe") {
        terminal.reset(new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read));
        QObject::connect(terminal.get(), &QSocketNotifier::activated, &app, [&](int) {
            char bytes[32]; const auto count = read(STDIN_FILENO, bytes, sizeof(bytes));
            if (count <= 0) { terminal->setEnabled(false); cancel(); return; }
            for (ssize_t i = 0; i < count; ++i)
                if (bytes[i] == 'q' || bytes[i] == 'Q' || bytes[i] == 's' || bytes[i] == 'S') cancel();
        });
    }
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&]() { failed = true; cancel(); });
    QTimer::singleShot(0, &app, [&]() {
        // Startup includes PowerShell compilation; BLE work has its own helper deadline.
        deadline.start((seconds + 20) * 1000);
        QString executable = qEnvironmentVariable("TREADMILL_POWERSHELL", "powershell.exe");
        process.start(executable, arguments);
    });
    app.exec();
    collect(true); collect(false);
    if (!errors.isEmpty()) QTextStream(stderr) << QString::fromUtf8(errors);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(output, &parseError);
    const auto report = document.object();
    if (failed || process.exitStatus() != QProcess::NormalExit || parseError.error != QJsonParseError::NoError ||
        !document.isObject() || report.value("backend") != "windows-winrt" || report.value("command") != command ||
        !report.contains("motor_execution_enabled") || report.value("motor_execution_enabled") != false)
        return fail("Windows diagnostic bridge failed or returned an invalid report: " + process.errorString());
    if (measureSleepTimer && report.value("measure_sleep_timer") != true)
        return fail("Windows helper did not confirm sleep-timer mode");
    if (statusDataSport && report.value("status_data_sport") != true)
        return fail("Windows helper did not confirm the requested current-exercise-data query");
    if (statusHeartRateZero && report.value("status_heart_rate_zero") != true)
        return fail("Windows helper did not confirm the requested status query variant");
    const auto bytes = document.toJson();
    if (log.isOpen() && (log.write(bytes) != bytes.size() || !log.flush()))
        return fail("Diagnostic recording failed: " + log.errorString());
    QTextStream(stdout) << QString::fromUtf8(bytes);
    const bool cleanupOk = command != "probe" || report.value("notification_cleanup_status") == "Success";
    const bool ftmsOk = !observeFtms || report.value("ftms_observation_status") == "complete";
    return process.exitCode() == 0 && report.value("status") == "complete" && cleanupOk && ftmsOk && !cancelled ? 0 : 1;
}
