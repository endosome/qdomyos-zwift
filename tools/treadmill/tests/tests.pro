QT = core
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = treadmill-tests
INCLUDEPATH += .. ../linux ../../../src/devices/fitshowtreadmill
!isEmpty(GTEST_INCLUDE): INCLUDEPATH += $$GTEST_INCLUDE
!isEmpty(GTEST_LIB): LIBS += -L$$GTEST_LIB
LIBS += -lgtest -lpthread
SOURCES += test_main.cpp test_motorgate.cpp ../controller.cpp ../workout.cpp ../profile.cpp ../fitshowcontroller.cpp ../runner.cpp
HEADERS += ../linux/motorgate.h ../controller.h ../workout.h ../profile.h ../fitshowcontroller.h ../runner.h

SOURCES += ../../../src/devices/fitshowtreadmill/fitshowcontrol.cpp
HEADERS += ../../../src/devices/fitshowtreadmill/fitshowcontrol.h
