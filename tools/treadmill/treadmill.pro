QT = core bluetooth
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = treadmill
SOURCES += main.cpp diagnostics.cpp controller.cpp workout.cpp
HEADERS += diagnostics.h controller.h workout.h
