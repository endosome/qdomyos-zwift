#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>
#include <algorithm>

static QJsonArray deviceReport(const QList<QBluetoothDeviceInfo> &discovered) {
    QList<QBluetoothDeviceInfo> devices;
    for (const auto &device : discovered) {
        if (device.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration)
            devices.append(device);
    }
    std::sort(devices.begin(), devices.end(), [](const auto &left, const auto &right) {
        return left.address().toString() < right.address().toString();
    });
    QJsonArray report;
    for (const auto &device : devices) {
        QStringList services;
        for (const auto &uuid : device.serviceUuids())
            services.append(uuid.toString());
        services.sort();
        QJsonArray advertisedServices;
        for (const auto &service : services)
            advertisedServices.append(service);
        QJsonObject entry;
        entry.insert(QStringLiteral("name"), device.name());
        entry.insert(QStringLiteral("address"), device.address().toString());
        entry.insert(QStringLiteral("rssi_dbm"), device.rssi());
        entry.insert(QStringLiteral("advertised_service_uuids"), advertisedServices);
        report.append(entry);
    }
    return report;
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("treadmill"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Read-only Linux BLE treadmill diagnostics."));
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("scan: list nearby BLE devices."));
    QCommandLineOption timeoutOption(QStringLiteral("timeout"),
                                    QStringLiteral("Scan duration in seconds (1–60)."),
                                    QStringLiteral("seconds"), QStringLiteral("10"));
    parser.addOption(timeoutOption);
    parser.process(app);
    const auto arguments = parser.positionalArguments();
    if (arguments.size() != 1 || arguments.first() != QStringLiteral("scan")) {
        QTextStream(stderr) << "Usage: treadmill scan [--timeout <seconds>]\n";
        return 2;
    }
    bool validTimeout = false;
    const int timeoutSeconds = parser.value(timeoutOption).toInt(&validTimeout);
    if (!validTimeout || timeoutSeconds < 1 || timeoutSeconds > 60) {
        QTextStream(stderr) << "--timeout must be an integer between 1 and 60 seconds.\n";
        return 2;
    }

    QBluetoothDeviceDiscoveryAgent discovery;
    discovery.setLowEnergyDiscoveryTimeout(timeoutSeconds * 1000);
    QTimer deadline;
    deadline.setSingleShot(true);
    bool completed = false;
    auto finish = [&](int exitCode, const QString &status, const QString &error) {
        if (completed)
            return;
        completed = true;
        deadline.stop();
        QJsonObject report;
        report.insert(QStringLiteral("command"), QStringLiteral("scan"));
        report.insert(QStringLiteral("status"), status);
        report.insert(QStringLiteral("devices"), deviceReport(discovery.discoveredDevices()));
        if (!error.isEmpty()) {
            report.insert(QStringLiteral("error"), error);
            QTextStream(stderr) << "BLE scan failed: " << error << '\n';
        }
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (discovery.isActive())
            discovery.stop();
        app.exit(exitCode);
    };
    QObject::connect(&discovery, &QBluetoothDeviceDiscoveryAgent::finished, &app, [&]() {
        finish(0, QStringLiteral("complete"), QString());
    });
    QObject::connect(&discovery, &QBluetoothDeviceDiscoveryAgent::canceled, &app, [&]() {
        finish(1, QStringLiteral("canceled"), QStringLiteral("Discovery was canceled."));
    });
    QObject::connect(&discovery,
                     QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(&QBluetoothDeviceDiscoveryAgent::error),
                     &app, [&](QBluetoothDeviceDiscoveryAgent::Error) {
        finish(1, QStringLiteral("error"), discovery.errorString());
    });
    QObject::connect(&deadline, &QTimer::timeout, &app, [&]() {
        finish(1, QStringLiteral("timeout"),
               QStringLiteral("Discovery did not finish within its deadline; results may be incomplete."));
    });
    // Start after the event loop begins so synchronous discovery errors can exit it.
    QTimer::singleShot(0, &app, [&]() {
        deadline.start(timeoutSeconds * 1000 + 1000);
        discovery.start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    });
    return app.exec();
}
