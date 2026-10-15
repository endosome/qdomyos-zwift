QT = core bluetooth
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = treadmill
SOURCES += main.cpp diagnostics.cpp controller.cpp workout.cpp profile.cpp runner.cpp session.cpp windowsbackend.cpp
HEADERS += diagnostics.h controller.h workout.h profile.h runner.h session.h windowsbackend.h
SOURCES += motortrial.cpp
HEADERS += motortrial.h

DEFINES += TREADMILL_WORKOUT_DIR=\\\"$$PWD/workouts\\\"
DEFINES += TREADMILL_WINDOWS_HELPER=\\\"$$PWD/windows-diagnostics.ps1\\\"

isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
workouts.files = $$PWD/workouts/*.xml
workouts.path = $$PREFIX/share/treadmill/workouts
documentation.files = $$PWD/README.md $$PWD/STATUS.md $$PWD/API.md $$PWD/../../LICENSE
documentation.path = $$PREFIX/share/doc/treadmill
INSTALLS += target workouts documentation

INCLUDEPATH += ../../src/devices/fitshowtreadmill

profiles.files = $$PWD/profiles/*.json
profiles.path = $$PREFIX/share/treadmill/profiles
INSTALLS += profiles
windowshelper.files = $$PWD/windows-diagnostics.ps1 $$PWD/windows-diagnostics.cs
windowshelper.path = $$PREFIX/share/treadmill/windows
INSTALLS += windowshelper

include(qzfitshow.pri)
INCLUDEPATH += .
SOURCES += fitshowcontroller.cpp driver/qzcontroller.cpp probe.cpp
HEADERS += fitshowcontroller.h driver/qzcontroller.h terminal.h probe.h
