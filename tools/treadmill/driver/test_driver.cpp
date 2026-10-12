#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include "fitshowtreadmill/fitshowtreadmill.h"

TEST(Driver, RestrictedModeRejectsLegacyControlAndNeverWritesBeforeTransport) {
    QSettings settings;
    settings.setValue(QZSettings::atletica_lightspeed_treadmill, true);
    settings.setValue(QZSettings::virtual_device_enabled, true);
    fitshowtreadmill driver(200, true, true, 0, 0, fitshowtreadmill::Mode::Restricted);
    int faults = 0;
    QObject::connect(&driver.restrictedControl(), &FitShow::Control::fault, [&](QString) { ++faults; });
    EXPECT_FALSE(driver.connected());
    EXPECT_FALSE(driver.autoStartWhenSpeedIsGreaterThenZero());
    EXPECT_FALSE(driver.autoPauseWhenSpeedIsZero());
    EXPECT_FALSE(driver.enableRestrictedDiagnostics());
    EXPECT_FALSE(driver.restrictedControl().limits().complete());
    driver.start(); driver.stop(false); driver.changeSpeed(3); driver.changeInclination(2, 2);
    driver.changeSpeedAndInclination(3, 2);
    EXPECT_EQ(faults, 5);
    EXPECT_TRUE(QMetaObject::invokeMethod(&driver, "update", Qt::DirectConnection));
    EXPECT_FALSE(driver.restrictedControl().transportAvailable());
}
TEST(Driver, MappingMustMatchAnExistingFitShowFamily) {
    fitshowtreadmill driver(200, true, true, 0, 0, fitshowtreadmill::Mode::Restricted);
    EXPECT_FALSE(driver.setRestrictedGatt(0xfff0, 0xffe1, 0xfff1));
    EXPECT_TRUE(driver.setRestrictedGatt(0xffe0, 0xffe1, 0xffe4));
    EXPECT_TRUE(driver.setRestrictedGatt(0xfff0, 0xfff2, 0xfff1));
    EXPECT_TRUE(driver.setRestrictedGatt(0xae00, 0xae01, 0xae02));
}
TEST(Driver, LegacyModeKeepsItsDefaultAutoStartPausePolicy) {
    fitshowtreadmill driver;
    EXPECT_TRUE(driver.autoStartWhenSpeedIsGreaterThenZero());
    EXPECT_TRUE(driver.autoPauseWhenSpeedIsZero());
    EXPECT_FALSE(driver.setRestrictedGatt(0xfff0, 0xfff2, 0xfff1));
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) return 1;
    QCoreApplication::setOrganizationName("treadmill-driver-tests");
    QCoreApplication::setApplicationName("offline");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
