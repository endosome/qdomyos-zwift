#pragma once
#include <QCommandLineParser>
#include <QCoreApplication>
void addWorkoutOptions(QCommandLineParser &parser);
int workoutCommand(QCoreApplication &app, const QCommandLineParser &parser, const QStringList &arguments);

int simulatorStatus();
