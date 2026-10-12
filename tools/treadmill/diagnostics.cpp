#include "diagnostics.h"
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
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

int scanDevices(QCoreApplication &app, int timeoutSeconds) {
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

#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyDescriptor>
#include <functional>

int inspectDevice(QCoreApplication &app, const QBluetoothAddress &address, int timeoutSeconds) {
    QBluetoothDeviceDiscoveryAgent discovery;
    discovery.setLowEnergyDiscoveryTimeout(timeoutSeconds * 1000);
    QTimer deadline;
    deadline.setSingleShot(true);
    QLowEnergyController *controller = nullptr;
    QList<QBluetoothUuid> pendingServices;
    QJsonArray services;
    QString deviceName;
    bool completed = false;
    auto finish = [&](const QString &error) {
        if (completed) return;
        completed = true;
        deadline.stop();
        QJsonObject report;
        report.insert("command", "info");
        report.insert("status", error.isEmpty() ? "complete" : "error");
        report.insert("address", address.toString());
        report.insert("name", deviceName);
        report.insert("services", services);
        report.insert("motor_capabilities", "unknown: no FitShow queries were sent");
        if (!error.isEmpty()) {
            report.insert("error", error);
            QTextStream(stderr) << "BLE inspection failed: " << error << '\n';
        }
        QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (discovery.isActive()) discovery.stop();
        if (controller) controller->disconnectFromDevice();
        app.exit(error.isEmpty() ? 0 : 1);
    };
    std::function<void()> discoverNext;
    discoverNext = [&]() {
        if (completed) return;
        if (pendingServices.isEmpty()) {
            finish(QString());
            return;
        }
        const auto uuid = pendingServices.takeFirst();
        auto *service = controller->createServiceObject(uuid, controller);
        if (!service) {
            finish(QStringLiteral("Cannot create service object: ") + uuid.toString());
            return;
        }
        QObject::connect(service, QOverload<QLowEnergyService::ServiceError>::of(&QLowEnergyService::error),
                         &app, [&, uuid](QLowEnergyService::ServiceError) {
            finish(QStringLiteral("Cannot inspect service: ") + uuid.toString());
        });
        QObject::connect(service, &QLowEnergyService::stateChanged, &app,
                         [&, service, uuid](QLowEnergyService::ServiceState state) {
            if (completed || state != QLowEnergyService::ServiceDiscovered) return;
            QJsonArray characteristics;
            auto list = service->characteristics();
            std::sort(list.begin(), list.end(), [](const auto &left, const auto &right) {
                return left.handle() < right.handle();
            });
            for (const auto &characteristic : list) {
                QJsonArray properties;
                const auto flags = characteristic.properties();
                if (flags & QLowEnergyCharacteristic::Read) properties.append("read");
                if (flags & QLowEnergyCharacteristic::Write) properties.append("write");
                if (flags & QLowEnergyCharacteristic::WriteNoResponse) properties.append("write_without_response");
                if (flags & QLowEnergyCharacteristic::Notify) properties.append("notify");
                if (flags & QLowEnergyCharacteristic::Indicate) properties.append("indicate");
                QJsonArray descriptors;
                for (const auto &descriptor : characteristic.descriptors()) {
                    QJsonObject entry;
                    entry.insert("uuid", descriptor.uuid().toString());
                    entry.insert("handle", descriptor.handle());
                    descriptors.append(entry);
                }
                QJsonObject entry;
                entry.insert("uuid", characteristic.uuid().toString());
                entry.insert("handle", characteristic.handle());
                entry.insert("properties", properties);
                entry.insert("descriptors", descriptors);
                const quint16 id = characteristic.uuid().toUInt16();
                if (uuid == QBluetoothUuid(quint16(0x180a)) &&
                    (id == 0x2a24 || id == 0x2a26 || id == 0x2a27 || id == 0x2a29) &&
                    (flags & QLowEnergyCharacteristic::Read)) {
                    entry.insert("device_information", characteristic.value().isEmpty()
                        ? QJsonValue() : QJsonValue(QString::fromUtf8(characteristic.value())));
                }
                characteristics.append(entry);
            }
            QJsonObject entry;
            entry.insert("uuid", uuid.toString());
            entry.insert("characteristics", characteristics);
            services.append(entry);
            QTimer::singleShot(0, &app, discoverNext);
        });
        // Qt 5.15 may read values during detail discovery; no writes/subscriptions.
        service->discoverDetails();
    };
    QObject::connect(&deadline, &QTimer::timeout, &app, [&]() {
        finish(QStringLiteral("Discovery/inspection deadline exceeded; report may be incomplete."));
    });
    QObject::connect(&discovery,
                     QOverload<QBluetoothDeviceDiscoveryAgent::Error>::of(&QBluetoothDeviceDiscoveryAgent::error),
                     &app, [&](QBluetoothDeviceDiscoveryAgent::Error) { finish(discovery.errorString()); });
    QObject::connect(&discovery, &QBluetoothDeviceDiscoveryAgent::canceled, &app, [&]() {
        finish(QStringLiteral("Discovery was canceled."));
    });
    QObject::connect(&discovery, &QBluetoothDeviceDiscoveryAgent::finished, &app, [&]() {
        if (completed) return;
        QBluetoothDeviceInfo selected;
        for (const auto &device : discovery.discoveredDevices()) {
            if (device.address() == address &&
                (device.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration))
                selected = device;
        }
        if (!selected.isValid()) {
            finish(QStringLiteral("Selected BLE address was not found. Run scan and select its address explicitly."));
            return;
        }
        deviceName = selected.name();
        controller = QLowEnergyController::createCentral(selected, &app);
        QObject::connect(controller, QOverload<QLowEnergyController::Error>::of(&QLowEnergyController::error),
                         &app, [&](QLowEnergyController::Error) { finish(controller->errorString()); });
        QObject::connect(controller, &QLowEnergyController::disconnected, &app, [&]() {
            finish(QStringLiteral("Device disconnected during inspection."));
        });
        QObject::connect(controller, &QLowEnergyController::connected, controller,
                         &QLowEnergyController::discoverServices);
        QObject::connect(controller, &QLowEnergyController::discoveryFinished, &app, [&]() {
            pendingServices = controller->services();
            std::sort(pendingServices.begin(), pendingServices.end(), [](const auto &left, const auto &right) {
                return left.toString() < right.toString();
            });
            discoverNext();
        });
        deadline.start(30000);
        controller->connectToDevice();
    });
    QTimer::singleShot(0, &app, [&]() {
        deadline.start(timeoutSeconds * 1000 + 1000);
        discovery.start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    });
    const int result = app.exec();
    delete controller;
    return result;
}
