#include "probe.h"
#include "driver/qzcontroller.h"
#include "terminal.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>

int probeStatus(QCoreApplication &app, const QCommandLineParser &parser) {
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    if (!parser.isSet("profile") || !parser.isSet("confirm-diagnostics"))
        return fail("Protocol status requires --profile and --confirm-diagnostics. Notification/info/status writes require explicit approval.");
    auto loaded = Treadmill::loadProfile(parser.value("profile"));
    if (!loaded.error.isEmpty()) return fail(loaded.error);
    if (!loaded.profile.diagnosticsConfirmed) return fail("Profile diagnostic_writes_confirmed must be true after protocol review");
    bool validDuration = false;
    const int seconds = parser.isSet("timeout") ? parser.value("timeout").toInt(&validDuration) : (validDuration = true, 30);
    if (!validDuration || seconds < 1 || seconds > 60) return fail("Status --timeout must be 1–60 seconds");
    if (!isatty(STDIN_FILENO)) return fail("Protocol diagnostics require an interactive terminal confirmation");
    QTextStream(stderr) << "FitShow compatibility is unverified. This probe subscribes to notifications and writes only\n"
                          "QZ info queries 50/02, 50/03 and status query 51. It records raw data without\n"
                          "claiming measured movement. Use the physical controls for belt operation.\n"
                          "Device: " << loaded.profile.name << " (" << loaded.profile.address << ")\n"
                          "Type NONMOTOR to approve these writes: " << Qt::flush;
    if (QTextStream(stdin).readLine() != "NONMOTOR") return fail("Diagnostic writes were not confirmed");
    // This observer can never arm motor control, even if the supplied file claims verification.
    loaded.profile.controlsVerified = false;
    loaded.profile.pauseVerified = false;
    QFile log;
    if (parser.isSet("log")) {
        log.setFileName(parser.value("log"));
        if (!log.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return fail("Cannot create diagnostic log: " + log.errorString());
    }
    InputGuard input(true);
    if (!input.valid()) return fail("Cannot install diagnostic signal handling");
    Treadmill::QzController controller(loaded.profile);
    QElapsedTimer clock;
    clock.start();
    bool done = false;
    int result = 0;
    auto finish = [&]() {
        if (done) return;
        done = true;
        controller.disconnectDevice();
        app.quit();
    };
    auto record = [&](const QString &type, QJsonObject data) {
        data.insert("event", type);
        data.insert("monotonic_ms", clock.elapsed());
        data.insert("utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        data.insert("motor_execution_enabled", false);
        const auto line = QJsonDocument(data).toJson(QJsonDocument::Compact) + '\n';
        QTextStream(stdout) << line;
        if (log.isOpen() && (log.size() + line.size() > 8 * 1024 * 1024 || log.write(line) != line.size())) {
            result = 1;
            QTextStream(stderr) << "Diagnostic recording failed; closing connection.\n";
            finish();
        }
    };
    QObject::connect(&controller, &Treadmill::Controller::fault, &app, [&](const QString &reason) {
        record("fault", {{"reason", reason}}); result = 1; finish();
    });
    QObject::connect(&controller, &Treadmill::Controller::connectionChanged, &app, [&](bool connected) {
        record("connection", {{"connected", connected}});
        if (!connected && !done) { result = 1; finish(); }
    });
    QObject::connect(&controller, &Treadmill::FitShowController::identityObserved, &app,
                     [&](const QString &model, const QString &firmware) { record("identity", {{"model", model}, {"firmware", firmware}}); });
    QObject::connect(&controller, &Treadmill::FitShowController::frameObserved, &app, [&](const QByteArray &frame) {
        record("notification", {{"frame_hex", QString::fromLatin1(frame.toHex())}, {"valid_fitshow_response", FitShow::decodeResponse(frame).valid}});
    });
    QObject::connect(&controller.diagnosticChannel(), &FitShow::Control::payloadReady, &app, [&](const QByteArray &payload, quint64, quint64 id) {
        record("payload_requested", {{"payload_hex", QString::fromLatin1(payload.toHex())}, {"command_id", QString::number(id)}});
    });
    QObject::connect(&controller.diagnosticChannel(), &FitShow::Control::limitsChanged, &app, [&](FitShow::Limits limits) {
        record("raw_limits", {{"speed_min_code", limits.minSpeed}, {"speed_max_code", limits.maxSpeed}, {"unit_tag", limits.unit},
                              {"incline_min_code", limits.minIncline}, {"incline_max_code", limits.maxIncline}, {"pause_flag", limits.pause}});
    });
    QObject::connect(&controller.diagnosticChannel(), &FitShow::Control::statusChanged, &app, [&](FitShow::Status status) {
        record("raw_status", {{"status_code", status.state}, {"has_motor_data", status.hasMotorData},
                              {"speed_tenths_code", status.speedTenths}, {"incline_code", status.inclineCode}});
    });
    QObject::connect(&controller, &Treadmill::Controller::commandEvent, &app, [&](quint64 id, const QString &phase) {
        record("command", {{"command_id", QString::number(id)}, {"phase", phase}});
    });
    QSocketNotifier signalNotifier(input.signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32]; const auto ignored = read(input.signalPipe[0], bytes, sizeof(bytes)); (void)ignored;
        record("user", {{"action", "termination signal"}}); finish();
    });
    QSocketNotifier terminal(STDIN_FILENO, QSocketNotifier::Read);
    QObject::connect(&terminal, &QSocketNotifier::activated, &app, [&](int) {
        char bytes[32]; const auto count = read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count <= 0) { finish(); return; }
        for (ssize_t i = 0; i < count; ++i) if (bytes[i] == 'q' || bytes[i] == 'Q' || bytes[i] == 's' || bytes[i] == 'S') finish();
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [&]() { record("session_end", {{"reason", "diagnostic duration reached"}}); finish(); });
    QTimer::singleShot(0, &app, [&]() {
        record("session_start", {{"device", loaded.profile.address}, {"scope", "nonmotor diagnostics only"}});
        deadline.start(seconds * 1000);
        if (!done) controller.connectDevice(1);
    });
    app.exec();
    if (log.isOpen() && !log.flush()) result = 1;
    return result;
}
