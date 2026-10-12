#include "qzcontroller.h"
#include <QBluetoothAddress>
#include <QBluetoothDeviceInfo>
namespace Treadmill {
QzController::QzController(DeviceProfile profile, QObject *parent)
    : QzController(std::make_shared<Transport>(), std::move(profile), parent) {}
QzController::QzController(std::shared_ptr<Transport> transport, DeviceProfile profile, QObject *parent)
    : FitShowController(transport->driver.restrictedControl(), profile,
                        [transport]() { return transport->clock.elapsed(); },
                        [transport, profile]() {
                            if (!transport->driver.setRestrictedGatt(quint16(profile.service), quint16(profile.write), quint16(profile.notify))) {
                                emit transport->driver.restrictedControl().fault("Invalid/already configured GATT transport"); return;
                            }
                            QBluetoothDeviceInfo device(QBluetoothAddress(profile.address), profile.name, 0);
                            device.setCoreConfigurations(QBluetoothDeviceInfo::LowEnergyCoreConfiguration);
                            transport->driver.deviceDiscovered(device);
                        },
                        [transport]() { transport->driver.disconnectRestricted(); }, parent), transport_(std::move(transport)) {
    connect(&transport_->driver, &fitshowtreadmill::restrictedGattReady, this, [this, profile]() {
        if (!profile.diagnosticsConfirmed || !transport_->driver.enableRestrictedDiagnostics())
            emit fault("Restricted notification subscription was not approved or failed");
    });
    connect(&transport_->driver, &fitshowtreadmill::restrictedFrameObserved, this, &FitShowController::frameObserved);
    connect(&transport_->driver, &fitshowtreadmill::restrictedIdentityObserved, this, &FitShowController::observeIdentity);
}
QzController::~QzController() { disconnectDevice(); }
}
