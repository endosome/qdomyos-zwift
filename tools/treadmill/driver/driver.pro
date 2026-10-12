QT = core
CONFIG += staticlib c++17
TEMPLATE = lib
TARGET = qz-fitshow
include(../qzfitshow.pri)

INCLUDEPATH += ..
SOURCES += qzcontroller.cpp ../fitshowcontroller.cpp ../profile.cpp ../controller.cpp
HEADERS += qzcontroller.h ../fitshowcontroller.h ../profile.h ../controller.h
