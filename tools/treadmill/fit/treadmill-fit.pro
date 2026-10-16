QT = core
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = treadmill-fit
SDK = $$PWD/../../../src/fit-sdk
INCLUDEPATH += $$SDK $$PWD $$PWD/..
SOURCES += treadmill-fit.cpp fitworkout.cpp $$files($$SDK/*.cpp)
HEADERS += fitworkout.h

isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
INSTALLS += target
