QT = core bluetooth
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = treadmill-linux-helper
SOURCES += linuxhelper.cpp
HEADERS += motorgate.h

isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/libexec/treadmill
INSTALLS += target
