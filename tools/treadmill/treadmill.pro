QT = core bluetooth
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = treadmill
SOURCES += main.cpp diagnostics.cpp controller.cpp workout.cpp runner.cpp session.cpp
HEADERS += diagnostics.h controller.h workout.h runner.h session.h

DEFINES += TREADMILL_WORKOUT_DIR=\\\"$$PWD/workouts\\\"
