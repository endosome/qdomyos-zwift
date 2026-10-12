#pragma once
#include <QCoreApplication>
#include <QBluetoothAddress>
int scanDevices(QCoreApplication &app, int timeoutSeconds);
int inspectDevice(QCoreApplication &app, const QBluetoothAddress &address, int timeoutSeconds);
