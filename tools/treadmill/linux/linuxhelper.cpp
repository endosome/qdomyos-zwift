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
#include <QPair>
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
// FTMS is observed only: notifications for Treadmill Data and Fitness Machine Status. The control point
// (0x2ad9) is never touched, and there is deliberately no FTMS write code in this helper.
const QBluetoothUuid kFtmsService(quint16(0x1826));
const QBluetoothUuid kFtmsData(quint16(0x2acd));
const QBluetoothUuid kFtmsStatus(quint16(0x2ada));
const QString kStopFrame = QStringLiteral("0253035003");

class Helper : public QObject {
  public:
    Helper(const QBluetoothAddress &address, const QString &name, const QString &trial, int seconds, int waitWakeSeconds, bool observeFtms,
           Treadmill::MotorGate gate)
        : address_(address), name_(name), trial_(trial), seconds_(seconds), waitWake_(waitWakeSeconds), observeFtms_(observeFtms), gate_(std::move(gate)) {}

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
    enum class State { Scanning, Connecting, Discovering, Identity, FitShow, Subscribing, Ftms, Running, Closing };

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
        info_ = info;
        openController();
    }

    // BlueZ sometimes refuses the first connect with "Unknown Error". Nothing has been written at that
    // point, so the connect phase alone is retried (three attempts); later errors still shut down.
    void openController() {
        state_ = State::Connecting;
        step("connect");
        controller_ = QLowEnergyController::createCentral(info_, this);
        controller_->setRemoteAddressType(QLowEnergyController::PublicAddress);
        connect(controller_, QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::error), this, [this]() {
            const QString reason = controller_->errorString();
            if (state_ == State::Connecting && ++connectAttempts_ < 3) {
                auto *failed = controller_;
                controller_ = nullptr;
                failed->disconnect(this);
                failed->deleteLater();
                QTimer::singleShot(1500, this, [this]() { if (state_ == State::Connecting) openController(); });
                return;
            }
            shutdown("Controller error: " + reason);
        });
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
        if (observeFtms_ && services.count(kFtmsService) != 1) { shutdown("FTMS observation requires exactly one FTMS service"); return; }
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
                [this](const QLowEnergyDescriptor &, const QByteArray &value) { onDescriptorWritten(fitshow_, value); });
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

    void becomeRunning() {
        state_ = State::Running; step("running"); readyAt_ = clock_.elapsed();
        emitLine("READY\t" + name_);
    }

    void onDescriptorWritten(QLowEnergyService *from, const QByteArray &value) {
        if (state_ == State::Subscribing && from == fitshow_) {
            if (observeFtms_) beginFtms(); else becomeRunning();
        } else if (state_ == State::Ftms && from == ftms_) {
            if (ftmsStep_ == 0) {
                ftmsStep_ = 1;
                ftms_->writeDescriptor(ftmsStatus_.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), QByteArray::fromHex("0100"));
            } else {
                ftmsEnabled_ = true;
                becomeRunning();
            }
        } else if (state_ == State::Closing && closeStep_ == CloseStep::Unsubscribe && value == QByteArray::fromHex("0000")) {
            unsubscribeNext();
        }
    }

    // Passive FTMS observation: discover the service, then enable notifications for 2ACD and 2ADA, one at a time.
    void beginFtms() {
        state_ = State::Ftms; step("discover_ftms_observation");
        ftms_ = controller_->createServiceObject(kFtmsService, this);
        if (!ftms_) { shutdown("Cannot create the FTMS service"); return; }
        connect(ftms_, QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::error), this, [this]() {
            if (state_ == State::Closing) { if (closeStep_ == CloseStep::Unsubscribe) { unsubscribeFailed_ = true; unsubscribeNext(); } }
            else shutdown("FTMS service error");
        });
        connect(ftms_, &QLowEnergyService::stateChanged, this, [this](QLowEnergyService::ServiceState s) {
            if (s == QLowEnergyService::ServiceDiscovered && state_ == State::Ftms) subscribeFtms();
        });
        connect(ftms_, &QLowEnergyService::characteristicChanged, this, [this](const QLowEnergyCharacteristic &c, const QByteArray &bytes) {
            // Qt can deliver other FTMS characteristics (it reads values itself, e.g. 2ad3); the host accepts only these two.
            if (!collecting_ || (c.uuid() != kFtmsData && c.uuid() != kFtmsStatus)) return;
            if (c.uuid() == kFtmsData) gate_.observeFtms(bytes, clock_.elapsed());
            emitLine("FTMS\t" + c.uuid().toString(QUuid::WithoutBraces) + '\t' + QString::fromLatin1(bytes.toHex()));
        });
        connect(ftms_, &QLowEnergyService::descriptorWritten, this,
                [this](const QLowEnergyDescriptor &, const QByteArray &value) { onDescriptorWritten(ftms_, value); });
        ftms_->discoverDetails();
    }

    void subscribeFtms() {
        ftmsData_ = ftms_->characteristic(kFtmsData);
        ftmsStatus_ = ftms_->characteristic(kFtmsStatus);
        for (const auto &c : {ftmsData_, ftmsStatus_}) {
            if (!c.isValid() || !(c.properties() & QLowEnergyCharacteristic::Notify) ||
                !c.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration).isValid()) {
                shutdown("FTMS observation needs Notify on 2ACD and 2ADA"); return;
            }
        }
        step("enable_ftms_notifications");
        ftmsSubscribed_ = true; // cleanup is attempted whenever enabling was requested
        ftmsStep_ = 0;
        ftms_->writeDescriptor(ftmsData_.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), QByteArray::fromHex("0100"));
    }

    void onFitShowError(QLowEnergyService::ServiceError error) {
        if (state_ == State::Closing) {
            if (closeStep_ == CloseStep::Stop) completeWrite("Failed: service error " + QString::number(int(error)));
            else if (closeStep_ == CloseStep::Unsubscribe) { unsubscribeFailed_ = true; unsubscribeNext(); }
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
        // Setup (scan, connect, identity) has its own 30 s budget, like the host's preflight limit. Once running,
        // the gate bounds the motor to the trial deadline and forces a STOP; this is only a backstop that leaves
        // room for the host's 30 s preflight (plus any wait-for-wake time) and 10 s post-stop observation after READY.
        if (state_ != State::Running && clock_.elapsed() >= 30000) { shutdown("Inspection deadline exceeded"); return; }
        if (state_ == State::Running && clock_.elapsed() - readyAt_ >= (qint64(seconds_) + waitWake_ + 45) * 1000) { shutdown(QString()); return; }
        if (state_ != State::Running) return;
        if (gate_.needsStop(clock_.elapsed())) {
            shutdown(gate_.stopReason().isEmpty() ? QString("Linux motor watchdog requested stop")
                                                  : "Linux motor watchdog requested stop: " + gate_.stopReason());
            return;
        }
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
            unsubscribeList_.clear();
            unsubscribeFailed_ = false;
            if (linked) {
                if (ftmsSubscribed_) { unsubscribeList_.append({ftms_, ftmsStatus_}); unsubscribeList_.append({ftms_, ftmsData_}); }
                if (subscribed_ && notify_.isValid()) unsubscribeList_.append({fitshow_, notify_});
            }
            if (!unsubscribeList_.isEmpty()) { unsubscribeNext(true); return; }
            if (subscribed_ || ftmsSubscribed_) cleanupStatus_ = "unconfirmed: link unavailable";
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

    // Disables each enabled notification in turn; success is reported only if every one was confirmed.
    void unsubscribeNext(bool first = false) {
        closeTimeout_.stop();
        if (!first && !unsubscribeList_.isEmpty()) unsubscribeList_.removeFirst();
        if (unsubscribeList_.isEmpty()) {
            cleanupStatus_ = unsubscribeFailed_ ? "unconfirmed: a notification could not be disabled" : "Success";
            advanceClose();
            return;
        }
        disconnect(&closeTimeout_, nullptr, nullptr, nullptr);
        closeTimeout_.setSingleShot(true);
        connect(&closeTimeout_, &QTimer::timeout, this, [this]() { onCloseTimeout(); });
        closeTimeout_.start(2000);
        const auto &item = unsubscribeList_.first();
        item.first->writeDescriptor(item.second.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), QByteArray::fromHex("0000"));
    }

    void onCloseTimeout() {
        if (closeStep_ == CloseStep::Stop) completeWrite("Failed: timeout");
        else if (closeStep_ == CloseStep::Unsubscribe) { unsubscribeFailed_ = true; unsubscribeNext(); }
        else if (closeStep_ == CloseStep::Disconnect) finish();
    }

    void finish() {
        if (closeStep_ == CloseStep::Done) return;
        closeStep_ = CloseStep::Done;
        if (status_ == "complete" && (subscribed_ || ftmsSubscribed_) && cleanupStatus_ != "Success") { status_ = "error"; if (error_.isEmpty()) error_ = "Notification cleanup unconfirmed"; }
        QJsonObject report{{"command", "control-test"}, {"backend", "linux-bluez"}, {"motor_execution_enabled", true},
                           {"status", status_}, {"address", address_.toString()}, {"name", name_}, {"trial", trial_},
                           {"last_operation", lastOperation_}, {"notification_cleanup_status", cleanupStatus_},
                           {"ftms_observation_status", !observeFtms_ ? "not requested" : ftmsEnabled_ ? "complete" : "failed"}};
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
    int seconds_, waitWake_;
    bool observeFtms_;
    Treadmill::MotorGate gate_;
    State state_ = State::Scanning;
    CloseStep closeStep_ = CloseStep::None;
    QElapsedTimer clock_;
    QTimer tick_, writeTimeout_, closeTimeout_;
    QBluetoothDeviceDiscoveryAgent agent_;
    QLowEnergyController *controller_ = nullptr;
    QLowEnergyService *identity_ = nullptr, *fitshow_ = nullptr, *ftms_ = nullptr;
    QLowEnergyCharacteristic notify_, write_, ftmsData_, ftmsStatus_;
    QList<QPair<QLowEnergyService *, QLowEnergyCharacteristic>> unsubscribeList_;
    int ftmsStep_ = 0;
    bool ftmsSubscribed_ = false, ftmsEnabled_ = false, unsubscribeFailed_ = false;
    std::unique_ptr<QSocketNotifier> stdinNotifier_;
    QSet<quint16> pendingIdentity_;
    QByteArray input_, writeId_;
    QString writeHex_, inputFault_, lastOperation_, status_ = "complete", error_, cleanupStatus_ = "not_enabled";
    QList<QByteArray> commands_, output_;
    int offset_ = 0;
    qint64 readyAt_ = 0;
    int connectAttempts_ = 0;
    QBluetoothDeviceInfo info_;
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
    parser.addOption({"wait-wake", "Extra seconds the host may wait for the console to wake before the start.", "seconds", "0"});
    parser.addOption({"observe-ftms", "Also stream FTMS Treadmill Data and Status notifications (observation only, no FTMS writes)."});
    parser.addOption({"profile", "Motor profile: walking (default, raw speed 10-60) or running (opt-in, 10-120, needs --observe-ftms).", "name", "walking"});
    parser.process(app);
    auto fail = [](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    const QBluetoothAddress address(parser.value("device"));
    const QString name = parser.value("expected-name");
    if (!parser.isSet("confirm-motor")) return fail("Motor control requires --confirm-motor from the CLI host");
    if (address.isNull() || name.trimmed().isEmpty()) return fail("--device and --expected-name are required");
    const QString trial = parser.value("trial");
    bool timeoutOk = false;
    const int seconds = parser.value("timeout").toInt(&timeoutOk);
    if (!timeoutOk || seconds < 1 || seconds > (trial == "workout" ? 9000 : 240)) return fail("Session duration exceeds its bounded scope");
    bool waitOk = false;
    const int waitWake = parser.value("wait-wake").toInt(&waitOk);
    if (!waitOk || waitWake < 0 || waitWake > 1800) return fail("--wait-wake must be 0-1800 seconds");
    const QString profileName = parser.value("profile");
    if (profileName != "walking" && profileName != "running") return fail("--profile must be walking or running");
    const Treadmill::Profile profile = profileName == "running" ? Treadmill::Profile::Running : Treadmill::Profile::Walking;
    if (profile == Treadmill::Profile::Running && !parser.isSet("observe-ftms")) return fail("The running profile requires --observe-ftms: FTMS is the independent speed check");
    std::unique_ptr<Helper> helper;
    try {
        helper.reset(new Helper(address, name, trial, seconds, waitWake, parser.isSet("observe-ftms"), Treadmill::MotorGate(trial, parser.value("target-sequence"), seconds, profile)));
    } catch (const std::exception &error) { return fail(QString::fromUtf8(error.what())); }
    QTimer::singleShot(0, &app, [&]() { helper->start(); });
    return app.exec();
}
