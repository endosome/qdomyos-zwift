# Clean Debian 13 build and offline runtime. No host Bluetooth/D-Bus mounts.
FROM debian:13@sha256:913f6706df59a68922d1dd08f78c2476560a8d367897200a6005b00e5f67c2d5 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential qtbase5-dev qt5-qmake qtconnectivity5-dev \
    qtpositioning5-dev libgtest-dev python3 \
    && rm -rf /var/lib/apt/lists/*
COPY LICENSE /source/LICENSE
COPY tools/treadmill /source/tools/treadmill
COPY src/metric.cpp src/metric.h src/qzsettings.cpp src/qzsettings.h \
    src/bluetoothdevicetype.h src/definitions.h src/qdebugfixup.h \
    src/sessionline.h src/ergtable.h src/treadmillErgTable.h \
    src/keepawakehelper.h /source/src/
COPY src/fit-sdk /source/src/fit-sdk
COPY src/devices/bluetoothdevice.cpp src/devices/bluetoothdevice.h \
    src/devices/treadmill.cpp src/devices/treadmill.h /source/src/devices/
COPY src/devices/fitshowtreadmill /source/src/devices/fitshowtreadmill
COPY src/virtualdevices/virtualdevice.cpp src/virtualdevices/virtualdevice.h /source/src/virtualdevices/
ENV DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/no-bluetooth-bus
WORKDIR /build/cli
RUN qmake /source/tools/treadmill/treadmill.pro && make -j2
WORKDIR /build/tests
RUN qmake /source/tools/treadmill/tests/tests.pro && make -j2 \
    && ./treadmill-tests --gtest_output=xml:/build/core-results.xml \
    && python3 /source/tools/treadmill/tests/test_cli.py /build/cli/treadmill \
    && python3 /source/tools/treadmill/tests/test_windows_bridge.py /build/cli/treadmill \
    && python3 /source/tools/treadmill/tests/test_motortrial.py /build/cli/treadmill
WORKDIR /build/helper
RUN qmake /source/tools/treadmill/linux/linux-helper.pro && make -j2 && python3 /source/tools/treadmill/tests/test_linux_helper.py /build/helper/treadmill-linux-helper
WORKDIR /build/driver
RUN qmake /source/tools/treadmill/driver/driver.pro && make -j2
WORKDIR /build/driver-tests
RUN qmake /source/tools/treadmill/driver/tests.pro DRIVER_LIB=/build/driver \
    && make -j2 && ./qz-fitshow-tests --gtest_output=xml:/build/driver-results.xml
WORKDIR /build/helper
RUN make INSTALL_ROOT=/stage install
WORKDIR /build/cli
RUN make INSTALL_ROOT=/stage install \
    && dpkg-query -W > /stage/usr/local/share/doc/treadmill/build-packages.txt \
    && tar -C /source -czf /stage/usr/local/share/doc/treadmill/corresponding-source.tar.gz .

# Export the installation tree with --target package --output type=local,dest=...
FROM scratch AS package
COPY --from=build /stage/ /

# Runtime has no source checkout or compiler; catalog must use installed XML.
FROM debian:13@sha256:913f6706df59a68922d1dd08f78c2476560a8d367897200a6005b00e5f67c2d5 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
    libqt5core5t64 libqt5bluetooth5 libqt5positioning5 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /stage/ /
ENV HOME=/tmp DBUS_SYSTEM_BUS_ADDRESS=unix:path=/tmp/no-bluetooth-bus
USER 65534:65534
ENTRYPOINT ["/usr/local/bin/treadmill"]
CMD ["workout", "list"]
