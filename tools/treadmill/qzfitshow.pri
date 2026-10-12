# Bounded QZ dependency closure. No device factory, GUI or virtual peripheral.
QT += bluetooth positioning
DEFINES += TREADMILL_NO_VIRTUAL_DEVICE
QZ_ROOT = $$clean_path($$PWD/../../src)
INCLUDEPATH += $$QZ_ROOT $$QZ_ROOT/devices $$QZ_ROOT/devices/fitshowtreadmill
SOURCES += \
    $$QZ_ROOT/devices/fitshowtreadmill/fitshowtreadmill.cpp \
    $$QZ_ROOT/devices/fitshowtreadmill/fitshowcontrol.cpp \
    $$QZ_ROOT/devices/treadmill.cpp \
    $$QZ_ROOT/devices/bluetoothdevice.cpp \
    $$QZ_ROOT/metric.cpp \
    $$QZ_ROOT/qzsettings.cpp \
    $$QZ_ROOT/virtualdevices/virtualdevice.cpp
HEADERS += \
    $$QZ_ROOT/devices/fitshowtreadmill/fitshowtreadmill.h \
    $$QZ_ROOT/devices/fitshowtreadmill/fitshowpacket.h \
    $$QZ_ROOT/devices/fitshowtreadmill/fitshowcontrol.h \
    $$QZ_ROOT/devices/treadmill.h \
    $$QZ_ROOT/devices/bluetoothdevice.h \
    $$QZ_ROOT/ergtable.h \
    $$QZ_ROOT/treadmillErgTable.h \
    $$QZ_ROOT/virtualdevices/virtualdevice.h
