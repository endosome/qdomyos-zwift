#include "controller.h"
#include "workout.h"
#include <QTemporaryFile>
#include <QFileInfo>
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
static WorkoutLoadResult parsePlan(const QByteArray &xml) {
    QTemporaryFile file;
    EXPECT_TRUE(file.open());
    file.write(xml);
    file.flush();
    return loadWorkout(file.fileName());
}
TEST(Workout, RepeatsPreserveRampEventsAndExactEndpoint) {
    auto result = parsePlan(R"(<rows version="1" id="a" name="A" speedunit="km/h" inclineunit="percent">
      <repeat times="2"><row name="Ramp" duration="00:00:10" speedfrom="1" speedto="2" inclination="0" forcespeed="1">
      <textevent timeoffset="5" message="One"/><textevent timeoffset="5" message="Two"/>
      </row></repeat></rows>)");
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_EQ(result.workout.stages.size(), 2);
    EXPECT_EQ(result.workout.durationMs(), 20000);
    EXPECT_EQ(result.workout.stages[0].events.size(), 2);
    EXPECT_DOUBLE_EQ(result.workout.stages[0].targetAt(10000).speedKmh, 2);
}
TEST(Workout, RejectsAmbiguousUnitsAndNestedRepeats) {
    EXPECT_FALSE(parsePlan(R"(<rows id="a" name="A"><row duration="00:01:00" speed="2" inclination="0"/></rows>)").error.isEmpty());
    EXPECT_FALSE(parsePlan(R"(<rows version="1" id="a" name="A" speedunit="km/h" inclineunit="percent"><repeat times="2"><repeat times="2"/></repeat></rows>)").error.isEmpty());
}
TEST(Workout, RejectsMalformedNumbersAndTrailingRoot) {
    EXPECT_FALSE(parsePlan(R"(<rows version="1" id="a" name="A" speedunit="km/h" inclineunit="percent"><row name="bad" duration="00:01:00" speed="nan" inclination="0" forcespeed="1"/></rows>)").error.isEmpty());
    EXPECT_FALSE(parsePlan(R"(<rows version="1" id="a" name="A" speedunit="km/h" inclineunit="percent"><row name="ok" duration="00:01:00" speed="2" inclination="0" forcespeed="1"/></rows><rows/>)").error.isEmpty());
}
TEST(Workout, ExampleDurationAndDifferentPlanValidate) {
    const QString directory = QFileInfo(__FILE__).absolutePath() + "/../workouts/";
    auto example = loadWorkout(directory + "uphill-walk-20.xml");
    auto other = loadWorkout(directory + "interval-demo.xml");
    ASSERT_TRUE(example.error.isEmpty()) << example.error.toStdString();
    ASSERT_TRUE(other.error.isEmpty()) << other.error.toStdString();
    EXPECT_EQ(example.workout.durationMs(), 1200000);
    EXPECT_EQ(other.workout.durationMs(), 120000);
    Simulator simulator;
    EXPECT_TRUE(validateWorkout(example.workout, simulator.capabilities()).isEmpty());
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
