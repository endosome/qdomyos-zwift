// BlueZ/Qt transport helper for the treadmill CLI's W1 control path.
//
// It speaks the same line protocol as windows-diagnostics.ps1 in control-test mode:
//   stdin : WRITE<TAB><id><TAB><hex frame>   |   QUIT (or EOF)
//   stdout: IDENTITY / READY / RX / TX lines, then one final JSON report line.
// Every motor write passes MotorGate, which enforces the allowlist, three fresh NORMAL
// preflight reports, the reviewed limits, a stale-status watchdog and the trial deadline.
// On EOF, a fault, stale status or the deadline it sends one STOP before closing.
#include "motorgate.h"
#include <QBluetoothAddress>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothUuid>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLowEnergyController>
#include <QLowEnergyDescriptor>
#include <QLowEnergyService>
#include <QSet>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

namespace {

const QBluetoothUuid kIdentityService(quint16(0x180a));
const QBluetoothUuid kFitShowService(quint16(0xfff0));
const QBluetoothUuid kFitShowNotify(quint16(0xfff1));
const QBluetoothUuid kFitShowWrite(quint16(0xfff2));
const QString kStopFrame = QStringLiteral("0253035003");

class Helper : public QObject {
  public:
    Helper(const QBluetoothAddress &address, const QString &name, const QString &trial, int seconds, Treadmill::MotorGate gate)
        : address_(address), name_(name), trial_(trial), seconds_(seconds), gate_(std::move(gate)) {}

    void start() {
        clock_.start();
        const int flags = fcntl(STDOUT_FILENO, F_GETFL);
        if (flags >= 0) fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
        stdinNotifier_.reset(new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read));
        connect(stdinNotifier_.get(), &QSocketNotifier::activated, this, &Helper::readStdin);
        connect(&tick_, &QTimer::timeout, this, &Helper::onTick);
        tick_.start(20);
        step("scan");
        connect(&agent_, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this, &Helper::onDiscovered);
        connect(&agent_, QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(&QBluetoothDeviceDiscoveryAgent::error), this,
                [this]() { shutdown("Discovery failed: " + agent_.errorString()); });
        connect(&agent_, &QBluetoothDeviceDiscoveryAgent::finished, this, [this]() {
            if (state_ == State::Scanning) shutdown("Selected device was not found by the BlueZ scan");
        });
        agent_.setLowEnergyDiscoveryTimeout(15000);
        agent_.start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    }

  private:
    enum class State { Scanning, Connecting, Discovering, Identity, FitShow, Subscribing, Running, Closing };

    void step(const QString &operation) { lastOperation_ = operation; }

    void emitLine(const QString &line) {
        if (output_.size() >= 4096) { overflow_ = true; return; }
        output_.append(line.toUtf8() + '\n');
        flushOutput();
    }

    // Non-blocking so a stalled parent cannot freeze the watchdog; overflow stops the run.
    void flushOutput() {
        while (!output_.isEmpty()) {
            const QByteArray &line = output_.first();
            const ssize_t written = write(STDOUT_FILENO, line.constData() + offset_, line.size() - offset_);
            if (written < 0) return;
            offset_ += int(written);
            if (offset_ >= line.size()) { output_.removeFirst(); offset_ = 0; }
        }
    }

    void onDiscovered(const QBluetoothDeviceInfo &info) {
        if (state_ != State::Scanning || info.address() != address_) return;
        if (info.name() != name_) { shutdown("Selected device name differs from the operator-confirmed name: " + info.name()); return; }
        agent_.stop();
        state_ = State::Connecting;
        step("connect");
        controller_ = QLowEnergyController::createCentral(info, this);
        controller_->setRemoteAddressType(QLowEnergyController::PublicAddress);
        connect(controller_, QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::error), this,
                [this]() { shutdown("Controller error: " + controller_->errorString()); });
        connect(controller_, &QLowEnergyController::disconnected, this, [this]() {
            disconnected_ = true;
            if (state_ != State::Closing) shutdown("Device disconnected");
            else advanceClose();
        });
        connect(controller_, &QLowEnergyController::connected, this, [this]() {
            state_ = State::Discovering; step("discover_services"); controller_->discoverServices();
        });
        connect(controller_, &QLowEnergyController::discoveryFinished, this, &Helper::onServices);
        controller_->connectToDevice();
    }

    void onServices() {
        if (state_ != State::Discovering) return;
        const auto services = controller_->services();
        if (services.count(kIdentityService) != 1) { shutdown("Module identity service unavailable; motor control remains blocked"); return; }
        if (services.count(kFitShowService) != 1) { shutdown("Expected exactly one accessible FFF0 service"); return; }
        state_ = State::Identity; step("verify_fitshow_module_identity");
        identity_ = controller_->createServiceObject(kIdentityService, this);
        if (!identity_) { shutdown("Cannot create identity service"); return; }
        connect(identity_, QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::error), this,
                [this]() { if (state_ == State::Identity) shutdown("Module identity characteristics unavailable"); });
        connect(identity_, &QLowEnergyService::stateChanged, this, [this](QLowEnergyService::ServiceState s) {
            if (s == QLowEnergyService::ServiceDiscovered && state_ == State::Identity) readIdentity();
        });
        connect(identity_, &QLowEnergyService::characteristicRead, this,
                [this](const QLowEnergyCharacteristic &c, const QByteArray &value) { onIdentityValue(c, value); });
        identity_->discoverDetails();
    }

    void readIdentity() {
        for (const quint16 id : {0x2a29, 0x2a24, 0x2a28}) {
            const auto characteristic = identity_->characteristic(QBluetoothUuid(id));
            if (!characteristic.isValid() || !(characteristic.properties() & QLowEnergyCharacteristic::Read)) {
                shutdown("Module identity value unreadable"); return;
            }
            pendingIdentity_.insert(id);
        }
        for (const quint16 id : {0x2a29, 0x2a24, 0x2a28}) identity_->readCharacteristic(identity_->characteristic(QBluetoothUuid(id)));
    }

    void onIdentityValue(const QLowEnergyCharacteristic &c, const QByteArray &value) {
        if (state_ != State::Identity) return;
        const quint16 id = c.uuid().toUInt16();
        if (!pendingIdentity_.remove(id)) return;
        const QString text = QString::fromUtf8(value).remove(QChar('\0'));
        if (id == 0x2a29 && text != "FITSHOW") { shutdown("Module identity differs from the reviewed W1 evidence"); return; }
        if (id == 0x2a24 && text != "FS-BT-C1") { shutdown("Module identity differs from the reviewed W1 evidence"); return; }
        if (id == 0x2a28 && text != "1.3.8") { shutdown("Module identity differs from the reviewed W1 evidence"); return; }
        if (!pendingIdentity_.isEmpty()) return;
        emitLine("IDENTITY\tFITSHOW\tFS-BT-C1\t1.3.8");
        state_ = State::FitShow; step("discover_fitshow_service");
        fitshow_ = controller_->createServiceObject(kFitShowService, this);
        if (!fitshow_) { shutdown("Cannot create FFF0 service"); return; }
        connect(fitshow_, QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::error), this,
                [this](QLowEnergyService::ServiceError e) { onFitShowError(e); });
        connect(fitshow_, &QLowEnergyService::stateChanged, this, [this](QLowEnergyService::ServiceState s) {
            if (s == QLowEnergyService::ServiceDiscovered && state_ == State::FitShow) subscribe();
        });
        connect(fitshow_, &QLowEnergyService::characteristicChanged, this, &Helper::onNotification);
        connect(fitshow_, &QLowEnergyService::characteristicWritten, this,
                [this](const QLowEnergyCharacteristic &c, const QByteArray &) { if (c.uuid() == kFitShowWrite) completeWrite("Success"); });
        connect(fitshow_, &QLowEnergyService::descriptorWritten, this,
                [this](const QLowEnergyDescriptor &, const QByteArray &value) { onDescriptorWritten(value); });
        fitshow_->discoverDetails();
    }

    void subscribe() {
        notify_ = fitshow_->characteristic(kFitShowNotify);
        write_ = fitshow_->characteristic(kFitShowWrite);
        if (!notify_.isValid() || !write_.isValid() || !(notify_.properties() & QLowEnergyCharacteristic::Notify) ||
            !(write_.properties() & QLowEnergyCharacteristic::WriteNoResponse)) {
            shutdown("Expected FFF1 Notify and FFF2 WriteWithoutResponse properties"); return;
        }
        const auto cccd = notify_.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration);
        if (!cccd.isValid()) { shutdown("FFF1 has no notification descriptor"); return; }
        state_ = State::Subscribing; step("enable_notifications");
        subscribed_ = true; // Cleanup is attempted whenever enabling was requested.
        fitshow_->writeDescriptor(cccd, QByteArray::fromHex("0100"));
    }

    void onDescriptorWritten(const QByteArray &value) {
        if (state_ == State::Subscribing) {
            state_ = State::Running; step("running");
            emitLine("READY\t" + name_);
        } else if (state_ == State::Closing && closeStep_ == CloseStep::Unsubscribe && value == QByteArray::fromHex("0000")) {
            cleanupStatus_ = "Success";
            advanceClose();
        }
    }

    void onFitShowError(QLowEnergyService::ServiceError error) {
        if (state_ == State::Closing) {
            if (closeStep_ == CloseStep::Stop) completeWrite("Failed: service error " + QString::number(int(error)));
            else if (closeStep_ == CloseStep::Unsubscribe) { cleanupStatus_ = "unconfirmed: service error"; advanceClose(); }
            return;
        }
        if (writeInFlight_) completeWrite("Failed: service error " + QString::number(int(error)));
        else shutdown("FFF0 service error " + QString::number(int(error)));
    }

    void onNotification(const QLowEnergyCharacteristic &c, const QByteArray &bytes) {
        if (c.uuid() != kFitShowNotify || !collecting_) return;
        gate_.observe(bytes, clock_.elapsed());
        emitLine("RX\t" + QString::fromLatin1(bytes.toHex()));
    }

    void readStdin() {
        char chunk[256];
        const ssize_t count = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (count <= 0) { stdinNotifier_->setEnabled(false); cancelled_ = true; return; }
        input_ += QByteArray(chunk, int(count));
        int end;
        while ((end = input_.indexOf('\n')) >= 0) {
            const QByteArray line = input_.left(end).trimmed();
            input_.remove(0, end + 1);
            if (line == "QUIT") { cancelled_ = true; return; }
            if (line.size() > 80 || commands_.size() >= 16) { inputFault_ = "Control bridge input exceeded its bounds"; return; }
            commands_.append(line);
        }
        if (input_.size() > 80) inputFault_ = "Control bridge input exceeded its bounds";
    }

    void onTick() {
        flushOutput();
        if (state_ == State::Closing) return;
        if (overflow_) { shutdown("Output queue overflow; closing connection"); return; }
        if (!inputFault_.isEmpty()) { shutdown(inputFault_); return; }
        if (cancelled_) { shutdown(QString()); return; }
        if (clock_.elapsed() >= qint64(seconds_) * 1000) {
            shutdown(state_ == State::Running ? QString() : "Inspection deadline exceeded");
            return;
        }
        if (state_ != State::Running) return;
        if (gate_.needsStop(clock_.elapsed())) { shutdown("Linux motor watchdog requested stop"); return; }
        if (writeInFlight_ || commands_.isEmpty()) return;
        processCommand(commands_.takeFirst());
    }

    void processCommand(const QByteArray &command) {
        const QList<QByteArray> parts = command.split('\t');
        bool idOk = false;
        if (parts.size() != 3 || parts[0] != "WRITE") { shutdown("Invalid control bridge command"); return; }
        parts[1].toULongLong(&idOk);
        if (!idOk) { shutdown("Invalid control bridge command"); return; }
        QByteArray frame;
        try { frame = gate_.approve(QString::fromLatin1(parts[2]), clock_.elapsed()); }
        catch (const std::exception &error) { shutdown(QString::fromUtf8(error.what())); return; }
        step("bounded_motor_frame " + QString::fromLatin1(parts[2]));
        beginWrite(parts[1], QString::fromLatin1(parts[2]), frame);
    }

    void beginWrite(const QByteArray &id, const QString &hex, const QByteArray &frame) {
        writeInFlight_ = true; writeId_ = id; writeHex_ = hex;
        writeTimeout_.setSingleShot(true);
        disconnect(&writeTimeout_, nullptr, nullptr, nullptr);
        // Qt's BlueZ backend never reports completion of a write-without-response, and the protocol has
        // no ACK for it. A write counts as accepted when BlueZ raises no error within the settle window (100 ms; the host writes about every 500 ms, bursts of 4 per second).
        connect(&writeTimeout_, &QTimer::timeout, this, [this]() { completeWrite("Success"); });
        writeTimeout_.start(100);
        fitshow_->writeCharacteristic(write_, frame, QLowEnergyService::WriteWithoutResponse);
    }

    void completeWrite(const QString &status) {
        if (!writeInFlight_) return;
        writeInFlight_ = false;
        writeTimeout_.stop();
        emitLine("TX\t" + QString::fromLatin1(writeId_) + '\t' + writeHex_ + '\t' + status);
        if (state_ == State::Closing) { advanceClose(); return; }
        if (status != "Success") shutdown("Control write failed: " + status);
    }

    // Shutdown: one emergency STOP if a start was attempted, CCCD cleanup, disconnect, final report.
    enum class CloseStep { None, Stop, Unsubscribe, Disconnect, Done };

    void shutdown(const QString &error) {
        if (state_ == State::Closing) return;
        if (!error.isEmpty() && error_.isEmpty()) { error_ = error; status_ = "error"; }
        state_ = State::Closing;
        collecting_ = true; // keep recording notifications until the stop is confirmed
        agent_.stop();
        commands_.clear();
        closeStep_ = CloseStep::None;
        advanceClose();
    }

    void advanceClose() {
        closeTimeout_.stop();
        disconnect(&closeTimeout_, nullptr, nullptr, nullptr);
        closeTimeout_.setSingleShot(true);
        connect(&closeTimeout_, &QTimer::timeout, this, [this]() { onCloseTimeout(); });
        const bool linked = controller_ && !disconnected_ && fitshow_ && write_.isValid();
        if (closeStep_ == CloseStep::None) {
            closeStep_ = CloseStep::Stop;
            if (gate_.started() && !gate_.stopAttempted()) {
                QString problem = "link unavailable";
                if (linked) {
                    try {
                        const QByteArray stop = gate_.approve(kStopFrame, clock_.elapsed());
                        step("emergency_stop");
                        beginWrite("0", kStopFrame, stop);
                        return;
                    } catch (const std::exception &failure) { problem = QString::fromUtf8(failure.what()); }
                }
                status_ = "error";
                error_ = (error_.isEmpty() ? QString() : error_ + "; ") + "Emergency stop unconfirmed: " + problem + ". Use the physical stop.";
            }
        }
        if (closeStep_ == CloseStep::Stop) {
            closeStep_ = CloseStep::Unsubscribe;
            collecting_ = false;
            if (subscribed_ && linked && notify_.isValid()) {
                cccdWrite();
                return;
            }
            if (subscribed_) cleanupStatus_ = "unconfirmed: link unavailable";
        }
        if (closeStep_ == CloseStep::Unsubscribe) {
            closeStep_ = CloseStep::Disconnect;
            if (controller_ && !disconnected_) {
                closeTimeout_.start(3000);
                controller_->disconnectFromDevice();
                return;
            }
        }
        if (closeStep_ == CloseStep::Disconnect) finish();
    }

    void cccdWrite() {
        closeTimeout_.start(2000);
        fitshow_->writeDescriptor(notify_.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), QByteArray::fromHex("0000"));
    }

    void onCloseTimeout() {
        if (closeStep_ == CloseStep::Stop) completeWrite("Failed: timeout");
        else if (closeStep_ == CloseStep::Unsubscribe) { cleanupStatus_ = "unconfirmed: timeout"; advanceClose(); }
        else if (closeStep_ == CloseStep::Disconnect) finish();
    }

    void finish() {
        if (closeStep_ == CloseStep::Done) return;
        closeStep_ = CloseStep::Done;
        if (status_ == "complete" && subscribed_ && cleanupStatus_ != "Success") { status_ = "error"; if (error_.isEmpty()) error_ = "Notification cleanup unconfirmed"; }
        QJsonObject report{{"command", "control-test"}, {"backend", "linux-bluez"}, {"motor_execution_enabled", true},
                           {"status", status_}, {"address", address_.toString()}, {"name", name_}, {"trial", trial_},
                           {"last_operation", lastOperation_}, {"notification_cleanup_status", cleanupStatus_}};
        if (!error_.isEmpty()) report.insert("error", error_);
        output_.append(QJsonDocument(report).toJson(QJsonDocument::Compact) + '\n');
        // Final flush must not be lost: block briefly for the parent to drain the pipe.
        const int flags = fcntl(STDOUT_FILENO, F_GETFL);
        if (flags >= 0) fcntl(STDOUT_FILENO, F_SETFL, flags & ~O_NONBLOCK);
        flushOutput();
        QCoreApplication::exit(status_ == "complete" ? 0 : 1);
    }

    QBluetoothAddress address_;
    QString name_, trial_;
    int seconds_;
    Treadmill::MotorGate gate_;
    State state_ = State::Scanning;
    CloseStep closeStep_ = CloseStep::None;
    QElapsedTimer clock_;
    QTimer tick_, writeTimeout_, closeTimeout_;
    QBluetoothDeviceDiscoveryAgent agent_;
    QLowEnergyController *controller_ = nullptr;
    QLowEnergyService *identity_ = nullptr, *fitshow_ = nullptr;
    QLowEnergyCharacteristic notify_, write_;
    std::unique_ptr<QSocketNotifier> stdinNotifier_;
    QSet<quint16> pendingIdentity_;
    QByteArray input_, writeId_;
    QString writeHex_, inputFault_, lastOperation_, status_ = "complete", error_, cleanupStatus_ = "not_enabled";
    QList<QByteArray> commands_, output_;
    int offset_ = 0;
    bool subscribed_ = false, collecting_ = true, writeInFlight_ = false, overflow_ = false, cancelled_ = false, disconnected_ = false;
};

} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("treadmill-linux-helper"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"device", "Exact Bluetooth address.", "address"});
    parser.addOption({"expected-name", "Exact operator-confirmed device name.", "name"});
    parser.addOption({"trial", "Control trial ID: start-stop, speed-N, incline-N or workout.", "id", "start-stop"});
    parser.addOption({"target-sequence", "Workout target pairs speed:incline,...", "pairs"});
    parser.addOption({"timeout", "Motor deadline in seconds.", "seconds", "120"});
    parser.addOption({"confirm-motor", "The CLI host obtained typed MOTOR approval."});
    parser.process(app);
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    const QBluetoothAddress address(parser.value("device"));
    const QString name = parser.value("expected-name");
    if (!parser.isSet("confirm-motor")) return fail("Motor control requires --confirm-motor from the CLI host");
    if (address.isNull() || name.trimmed().isEmpty()) return fail("--device and --expected-name are required");
    const QString trial = parser.value("trial");
    bool timeoutOk = false;
    const int seconds = parser.value("timeout").toInt(&timeoutOk);
    if (!timeoutOk || seconds < 1 || seconds > (trial == "workout" ? 4500 : 240)) return fail("Session duration exceeds its bounded scope");
    std::unique_ptr<Helper> helper;
    try {
        helper.reset(new Helper(address, name, trial, seconds, Treadmill::MotorGate(trial, parser.value("target-sequence"), seconds)));
    } catch (const std::exception &error) { return fail(QString::fromUtf8(error.what())); }
    QTimer::singleShot(0, &app, [&]() { helper->start(); });
    return app.exec();
}
