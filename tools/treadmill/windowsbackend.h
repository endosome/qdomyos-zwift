#pragma once
#include <QCoreApplication>
#include <QString>
QString windowsHelperPath(QString &error);

int windowsDiagnostics(QCoreApplication &app, const QString &command, int seconds,
                       const QString &address = {}, const QString &addressType = "unknown",
                       const QString &expectedName = {}, const QString &logPath = {}, bool observeFtms = false, bool statusHeartRateZero = false, bool statusDataSport = false, bool measureSleepTimer = false);
