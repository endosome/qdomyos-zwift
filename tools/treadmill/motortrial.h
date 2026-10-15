#pragma once
#include <QCoreApplication>
#include <QCommandLineParser>
#include "workout.h"
int motorTrial(QCoreApplication &app, const QCommandLineParser &parser);
int motorBatch(QCoreApplication &app, const QCommandLineParser &parser);
int runWindowsWorkout(QCoreApplication &app, const QCommandLineParser &parser, const Treadmill::Workout &workout);
