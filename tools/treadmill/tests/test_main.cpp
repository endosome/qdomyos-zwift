#include "controller.h"
#include <QCoreApplication>
#include <gtest/gtest.h>
using namespace Treadmill;

TEST(Simulator, AcknowledgementDoesNotMeanMovement) {
    Simulator simulator;
    Telemetry sample;
    QString phase;
    QObject::connect(&simulator, &Controller::sample, [&](Telemetry value) { sample = value; });
    QObject::connect(&simulator, &Controller::commandEvent, [&](quint64, QString value) { phase = value; });
    simulator.connectDevice(1);
    simulator.start(1, 1, {2, 0});
    simulator.advance(200);
    EXPECT_EQ(phase, "acknowledged");
    EXPECT_FALSE(sample.running);
    EXPECT_DOUBLE_EQ(sample.actual.speedKmh, 0);
}
TEST(Simulator, StopCancelsCountdownAndRejectsOldGeneration) {
    Simulator simulator;
    Telemetry sample;
    QObject::connect(&simulator, &Controller::sample, [&](Telemetry value) { sample = value; });
    simulator.connectDevice(1);
    simulator.start(1, 1, {2, 0});
    simulator.stop(2, 2);
    simulator.start(1, 3, {3, 5});
    simulator.advance(5000);
    EXPECT_FALSE(sample.running);
    EXPECT_DOUBLE_EQ(sample.actual.speedKmh, 0);
}
TEST(Simulator, StopFailureRetainsPhysicalMovement) {
    Simulator simulator;
    Telemetry sample;
    QObject::connect(&simulator, &Controller::sample, [&](Telemetry value) { sample = value; });
    simulator.connectDevice(1);
    simulator.start(1, 1, {2, 0});
    simulator.advance(4000);
    simulator.inject(Simulator::Failure::StopFailure);
    simulator.stop(2, 2);
    simulator.advance(6000);
    EXPECT_TRUE(sample.running);
    EXPECT_GT(sample.actual.speedKmh, 0);
}
TEST(Capabilities, RejectsUnknownAndNonfiniteTargets) {
    Capabilities unknown;
    EXPECT_FALSE(unknown.accepts({2, 0}));
    Simulator simulator;
    EXPECT_FALSE(simulator.capabilities().accepts({NAN, 0}));
    EXPECT_FALSE(simulator.capabilities().accepts({3, 11}));
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
