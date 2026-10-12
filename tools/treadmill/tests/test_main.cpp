#include "controller.h"
#include "workout.h"
#include "runner.h"
#include "fitshowpacket.h"
#include "fitshowcontrol.h"
#include "profile.h"
#include "fitshowcontroller.h"
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
static Workout shortPlan() {
    Workout workout;
    workout.id = "short";
    workout.name = "Short";
    Stage stage;
    stage.name = "Steady";
    stage.durationMs = 10000;
    stage.from = stage.to = {2, 0};
    workout.stages.append(stage);
    return workout;
}
static qint64 startRunner(Runner &runner) {
    runner.prepare(0);
    for (qint64 now = 200; now <= 10000; now += 200) {
        runner.advance(now);
        if (runner.state() == Runner::State::Ready) runner.confirmStart();
        if (runner.state() == Runner::State::Running) return now;
    }
    ADD_FAILURE() << "Runner failed to start";
    return 10000;
}
TEST(Runner, FullExampleCompletesWithVerifiedStop) {
    const QString path = QFileInfo(__FILE__).absolutePath() + "/../workouts/uphill-walk-20.xml";
    auto loaded = loadWorkout(path);
    ASSERT_TRUE(loaded.error.isEmpty());
    Simulator simulator;
    Runner runner(simulator, loaded.workout);
    qint64 now = startRunner(runner);
    while (!runner.finished() && now < 1500000) { now += 200; runner.advance(now); }
    EXPECT_EQ(runner.state(), Runner::State::Completed) << runner.reason().toStdString();
    EXPECT_EQ(runner.elapsedMs(), 1200000);
    EXPECT_DOUBLE_EQ(runner.telemetry().actual.speedKmh, 0);
}
TEST(Runner, AbortCancelsCountdownAndStops) {
    Simulator simulator;
    Runner runner(simulator, shortPlan());
    runner.prepare(0);
    runner.advance(200); runner.advance(400);
    ASSERT_EQ(runner.state(), Runner::State::Ready);
    runner.confirmStart();
    runner.abort("User stop");
    for (qint64 now = 600; now < 4000; now += 200) runner.advance(now);
    EXPECT_EQ(runner.state(), Runner::State::Aborted);
    EXPECT_EQ(runner.elapsedMs(), 0);
    EXPECT_FALSE(runner.telemetry().running);
}
TEST(Runner, StaleDataAndStopFailureCannotComplete) {
    for (const auto failure : {Simulator::Failure::StaleTelemetry, Simulator::Failure::StopFailure,
                               Simulator::Failure::Disconnect}) {
        Simulator simulator;
        Runner runner(simulator, shortPlan());
        qint64 now = startRunner(runner);
        simulator.inject(failure);
        runner.abort("Injected fault");
        for (int i = 0; i < 100 && !runner.finished(); ++i) { now += 200; runner.advance(now); }
        EXPECT_EQ(runner.state(), Runner::State::StopUnconfirmed);
    }
}
TEST(Runner, PauseFreezesTimeAndResumePreservesPosition) {
    Simulator simulator;
    Runner runner(simulator, shortPlan());
    qint64 now = startRunner(runner);
    now += 1000; runner.advance(now);
    const auto elapsed = runner.elapsedMs();
    runner.pause();
    for (int i = 0; i < 30; ++i) { now += 200; runner.advance(now); }
    ASSERT_EQ(runner.state(), Runner::State::Paused);
    EXPECT_EQ(runner.elapsedMs(), elapsed);
    runner.resume();
    for (int i = 0; i < 50 && !runner.finished(); ++i) { now += 200; runner.advance(now); }
    EXPECT_GT(runner.elapsedMs(), elapsed);
}
TEST(Runner, ClockGapAbortsInsteadOfCatchupAcceleration) {
    Simulator simulator;
    Runner runner(simulator, shortPlan());
    qint64 now = startRunner(runner);
    runner.advance(now + 5000);
    for (int i = 1; i < 30 && !runner.finished(); ++i) runner.advance(now + 5000 + i * 200);
    EXPECT_EQ(runner.state(), Runner::State::Aborted);
    EXPECT_EQ(runner.elapsedMs(), 0);
}
TEST(Workout, RejectsDtdDeclarations) {
    EXPECT_FALSE(parsePlan(R"(<!DOCTYPE rows [<!ENTITY name "Unexpected">]><rows version="1" id="a" name="&name;" speedunit="km/h" inclineunit="percent"><row name="ok" duration="00:01:00" speed="2" inclination="0" forcespeed="1"/></rows>)").error.isEmpty());
}
TEST(Runner, NoAckReportsTimeoutWithoutRepeatingStart) {
    Simulator simulator;
    simulator.inject(Simulator::Failure::NoAcknowledgements);
    Runner runner(simulator, shortPlan());
    int timedOut = 0;
    int submitted = 0;
    QObject::connect(&runner, &Runner::commandTimedOut, [&](quint64) { ++timedOut; });
    QObject::connect(&simulator, &Controller::commandEvent, [&](quint64, QString phase) {
        if (phase == "submitted") ++submitted;
    });
    qint64 now = startRunner(runner);
    while (!runner.finished() && now < 30000) { now += 200; runner.advance(now); }
    EXPECT_EQ(runner.state(), Runner::State::Completed);
    EXPECT_GT(timedOut, 0);
    EXPECT_EQ(submitted, 2); // One start, one stop; no blind retry.
}
TEST(Runner, ReductionCeilingSurvivesNextStage) {
    Simulator simulator;
    auto workout = shortPlan();
    Stage second = workout.stages[0];
    second.name = "Higher intensity";
    second.from = second.to = {3, 2};
    workout.stages.append(second);
    Runner runner(simulator, workout);
    qint64 now = startRunner(runner);
    runner.reduce();
    while (!runner.finished() && now < 50000) {
        now += 200; runner.advance(now);
        EXPECT_LE(runner.requested().speedKmh, 1.900001);
        EXPECT_LE(runner.requested().gradePercent, 0);
    }
    EXPECT_EQ(runner.state(), Runner::State::Completed);
}
TEST(Runner, NotificationsPreserveSameOffsetAndDoNotRepeat) {
    Simulator simulator;
    auto workout = shortPlan();
    workout.stages[0].durationMs = 40000;
    workout.stages[0].events = {{1000, "One"}, {1000, "Two"}};
    Runner runner(simulator, workout);
    QStringList messages;
    QObject::connect(&runner, &Runner::logEvent, [&](QString type, QString message) {
        if (type == "notification") messages.append(message);
    });
    qint64 now = startRunner(runner);
    while (!runner.finished() && now < 60000) { now += 200; runner.advance(now); }
    EXPECT_EQ(messages.count("One"), 1);
    EXPECT_EQ(messages.count("Two"), 1);
    EXPECT_EQ(messages.filter("Next in 30 s").size(), 1);
    EXPECT_EQ(messages.filter("Next in 10 s").size(), 1);
}
TEST(FitShowPacket, EncodesExistingPairedTargetFrame) {
    const uint8_t target[] = {0x53, 0x02, 30, 5};
    const auto frame = FitShowPacket::encode(target, sizeof(target));
    EXPECT_EQ(frame.toHex(), "0253021e054a03");
    EXPECT_TRUE(FitShowPacket::validResponse(reinterpret_cast<const uint8_t *>(frame.constData()), frame.size()));
}
TEST(FitShowPacket, RejectsTruncatedCorruptAndOversizedResponses) {
    const uint8_t status[] = {0x51};
    const auto shortFrame = FitShowPacket::encode(status, sizeof(status));
    EXPECT_FALSE(FitShowPacket::validResponse(reinterpret_cast<const uint8_t *>(shortFrame.constData()), shortFrame.size()));
    const uint8_t stop[] = {0x53, 0x03};
    const auto good = FitShowPacket::encode(stop, sizeof(stop));
    for (int i = 0; i < good.size(); ++i) {
        auto corrupt = good;
        corrupt[i] = char(uint8_t(corrupt[i]) ^ 1);
        EXPECT_FALSE(FitShowPacket::validResponse(reinterpret_cast<const uint8_t *>(corrupt.constData()), corrupt.size()));
    }
    const auto longFrame = good + QByteArray(256, char(0));
    EXPECT_FALSE(FitShowPacket::validResponse(reinterpret_cast<const uint8_t *>(longFrame.constData()), longFrame.size()));
    const uint8_t oversized[23] = {};
    EXPECT_TRUE(FitShowPacket::encode(oversized, sizeof(oversized)).isEmpty());
}
TEST(Simulator, CountdownTimeDoesNotAccelerateBelt) {
    Simulator simulator;
    Telemetry sample;
    QObject::connect(&simulator, &Controller::sample, [&](Telemetry value) { sample = value; });
    simulator.connectDevice(1);
    simulator.start(1, 1, {2, 0});
    simulator.advance(2000);
    EXPECT_TRUE(sample.running);
    EXPECT_DOUBLE_EQ(sample.actual.speedKmh, 0);
    simulator.advance(3000);
    EXPECT_DOUBLE_EQ(sample.actual.speedKmh, 0.5);
}
TEST(Simulator, ReconnectionDoesNotEraseExistingMovementOrStartAutomatically) {
    Simulator simulator;
    Telemetry sample;
    QObject::connect(&simulator, &Controller::sample, [&](Telemetry value) { sample = value; });
    simulator.connectDevice(1);
    simulator.start(1, 1, {2, 0});
    simulator.advance(6000);
    ASSERT_TRUE(sample.running);
    simulator.disconnectDevice();
    simulator.connectDevice(3);
    EXPECT_TRUE(sample.running);
    EXPECT_DOUBLE_EQ(sample.actual.speedKmh, 2);
    Runner newRun(simulator, shortPlan());
    newRun.prepare(6000);
    EXPECT_EQ(newRun.state(), Runner::State::Fault);
    EXPECT_EQ(newRun.reason(), "Initial treadmill state is moving");
}
static QByteArray wire(const char *hex) {
    const auto payload = QByteArray::fromHex(hex);
    return FitShowPacket::encode(reinterpret_cast<const uint8_t *>(payload.constData()), payload.size());
}
static void reportedLimits(FitShow::Control &control) {
    control.receive(wire("50023c05"));
    control.receive(wire("5003080002"));
}
TEST(FitShowControl, ReadOnlyDefaultAndUnknownLimitsRefuseMotorWrites) {
    FitShow::Control control;
    int writes = 0;
    QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray, quint64, quint64) { ++writes; });
    control.setTransport(true);
    control.queryLimits(1); control.pump(200);
    EXPECT_EQ(writes, 0);
    EXPECT_FALSE(control.arm(1));
    control.allowDiagnostics(true);
    EXPECT_FALSE(control.arm(1));
    reportedLimits(control);
    EXPECT_TRUE(control.arm(1));
}
TEST(FitShowControl, StopOvertakesInFlightStartAndCancelsInitialTarget) {
    FitShow::Control control;
    QList<QByteArray> writes;
    QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray payload, quint64, quint64) { writes.append(payload); });
    control.setTransport(true); control.allowDiagnostics(true); reportedLimits(control);
    ASSERT_TRUE(control.arm(1));
    control.start(1, 1, 20, 2); control.pump(200);
    control.stop(2, 2);
    control.receive(wire("5301")); // Late start acknowledgement cannot release an old target.
    control.target(1, 3, 30, 5);
    control.receive(wire("5303")); control.pump(400);
    ASSERT_GE(writes.size(), 2);
    EXPECT_EQ(writes[0].toHex(), "53010000000000000000");
    EXPECT_EQ(writes[1].toHex(), "5303");
    for (int i = 2; i < writes.size(); ++i) EXPECT_EQ(writes[i].toHex(), "51");
}
TEST(FitShowControl, AbortFromSubmissionCallbackCannotTransmitCanceledPayload) {
    FitShow::Control control;
    QList<QByteArray> writes;
    control.setTransport(true); control.allowDiagnostics(true); reportedLimits(control);
    ASSERT_TRUE(control.arm(1));
    QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray payload, quint64, quint64) { writes.append(payload); });
    QObject::connect(&control, &FitShow::Control::commandEvent, [&](quint64 id, QString phase) {
        if (id == 1 && phase == "submitted") control.stop(2, 2);
    });
    control.start(1, 1, 20, 2); control.pump(200);
    ASSERT_EQ(writes.size(), 1);
    EXPECT_EQ(writes[0].toHex(), "5303");
}
TEST(FitShowControl, CoalescesTargetsAndDoesNotRetryOnTimeout) {
    FitShow::Control control;
    QList<QByteArray> writes;
    control.setTransport(true); control.allowDiagnostics(true); reportedLimits(control);
    ASSERT_TRUE(control.arm(1));
    QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray payload, quint64, quint64) { writes.append(payload); });
    control.target(1, 1, 20, 0); control.target(1, 2, 30, 2); control.pump(200);
    ASSERT_EQ(writes.size(), 1);
    EXPECT_EQ(writes[0].toHex(), "53021e02");
    control.pump(2200); control.pump(2400);
    for (int i = 1; i < writes.size(); ++i) EXPECT_EQ(writes[i].toHex(), "51");
    control.target(1, 3, 30, 2); control.pump(2600);
    EXPECT_EQ(writes.size(), 2); // One target and one status poll, never another target.
}
TEST(FitShowControl, DisconnectClearsCapabilitiesAndAuthorization) {
    FitShow::Control control;
    control.setTransport(true); control.allowDiagnostics(true); reportedLimits(control);
    ASSERT_TRUE(control.arm(1));
    control.setTransport(false); control.setTransport(true);
    EXPECT_FALSE(control.limits().complete());
    EXPECT_FALSE(control.arm(3));
}
TEST(FitShowParser, PreservesNonRunningRawSpeedAndRejectsShortStatus) {
    auto response = FitShow::decodeResponse(wire("51040502000000000000000000"));
    ASSERT_TRUE(response.valid);
    EXPECT_TRUE(response.status.hasMotorData);
    EXPECT_EQ(response.status.speedTenths, 5); // Stopped status is not permission to invent speed zero.
    EXPECT_FALSE(FitShow::decodeResponse(wire("510400" )).valid);
    EXPECT_FALSE(FitShow::decodeResponse(wire("50053c050800" )).valid);
}
static DeviceProfile syntheticProfile() {
    DeviceProfile profile;
    profile.address = "AA:BB:CC:DD:EE:FF"; profile.name = "Synthetic fixture";
    profile.model = "test-model"; profile.firmware = "test-firmware"; profile.evidence = "offline fixture only";
    profile.service = 0xfff0; profile.write = 0xfff2; profile.notify = 0xfff1;
    profile.speedUnit = DeviceProfile::SpeedUnit::Kmh;
    profile.minSpeedCode = 5; profile.runningMinCode = 5; profile.maxSpeedCode = 60; profile.speedStepCode = 1;
    profile.inclines = {{0, 0}, {1, 1.5}, {2, 4}};
    profile.maxGradeMappingLoss = 2.5;
    profile.diagnosticsConfirmed = profile.controlsVerified = profile.speedFeedbackVerified =
        profile.inclineFeedbackVerified = profile.stopFeedbackVerified = true;
    return profile;
}
TEST(Profile, UsesCalibratedInclineTableAndExplicitMphScaling) {
    auto profile = syntheticProfile();
    profile.speedUnit = DeviceProfile::SpeedUnit::Mph;
    ASSERT_TRUE(profile.validate().isEmpty());
    Target mapped;
    ASSERT_TRUE(profile.map({2.0, 3.5}, mapped));
    EXPECT_DOUBLE_EQ(mapped.gradePercent, 1.5);
    EXPECT_LE(mapped.speedKmh, 2.0);
    int speed = -1, incline = -1;
    ASSERT_TRUE(profile.toWire(mapped, speed, incline));
    EXPECT_EQ(incline, 1);
    Target reported;
    ASSERT_TRUE(profile.fromWire(speed, incline, reported));
    EXPECT_NEAR(reported.speedKmh, mapped.speedKmh, 1e-7);
    EXPECT_DOUBLE_EQ(reported.gradePercent, 1.5);
    EXPECT_FALSE(profile.fromWire(speed, 8, reported));
}
TEST(Profile, IncompleteEvidenceCannotAuthorizeMotorAndBoundsMustMatch) {
    auto profile = syntheticProfile();
    profile.controlsVerified = false;
    EXPECT_FALSE(profile.capabilities().validated);
    profile.controlsVerified = true; profile.stopFeedbackVerified = false;
    EXPECT_FALSE(profile.validate().isEmpty());
    profile.stopFeedbackVerified = true;
    FitShow::Limits limits;
    limits.minSpeed = 5; limits.maxSpeed = 60; limits.minIncline = 0; limits.maxIncline = 2;
    EXPECT_TRUE(profile.matches(limits));
    limits.maxSpeed = 80;
    EXPECT_FALSE(profile.matches(limits));
}
TEST(Profile, RejectsRampLossBetweenOtherwiseValidEndpoints) {
    auto profile = syntheticProfile();
    profile.maxGradeMappingLoss = 1;
    auto workout = shortPlan();
    workout.stages[0].from.gradePercent = 0;
    workout.stages[0].to.gradePercent = 4;
    EXPECT_FALSE(validateWorkout(workout, profile.capabilities()).isEmpty());
    profile.maxGradeMappingLoss = 2.5;
    EXPECT_TRUE(validateWorkout(workout, profile.capabilities()).isEmpty());
}
TEST(Profile, ExampleRemainsUnverifiedAndMalformedEvidenceIsRejected) {
    const QString path = QFileInfo(__FILE__).absolutePath() + "/../profiles/unverified-example.json";
    auto loaded = loadProfile(path);
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    EXPECT_FALSE(loaded.profile.motorEvidenceComplete());
    EXPECT_FALSE(loaded.profile.capabilities().validated);
    QTemporaryFile file;
    ASSERT_TRUE(file.open());
    file.write("{\"version\":1,\"validation\":{\"controls\":true}}"); file.flush();
    EXPECT_FALSE(loadProfile(file.fileName()).error.isEmpty());
}
static QByteArray motorStatus(int state, int speed, int incline) {
    QByteArray payload;
    payload.append(char(0x51)); payload.append(char(state)); payload.append(char(speed)); payload.append(char(incline));
    payload.append(QByteArray(9, char(0)));
    return FitShowPacket::encode(reinterpret_cast<const uint8_t *>(payload.constData()), payload.size());
}
static void connectFixture(FitShowController &controller, FitShow::Control &control) {
    controller.connectDevice(1);
    control.pump(0); control.receive(wire("50023c05"));
    control.pump(200); control.receive(wire("5003020002"));
    controller.observeIdentity("test-model", "test-firmware");
}
TEST(FitShowAdapter, SendsMappedPairOnlyAfterLiveBoundsIdentityAndStoppedData) {
    FitShow::Control control;
    qint64 clock = 0;
    FitShowController controller(control, syntheticProfile(), [&]() { return clock; },
                                [&]() { control.setTransport(true); }, [&]() { control.setTransport(false); });
    QList<QByteArray> writes;
    QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray payload, quint64, quint64) { writes.append(payload); });
    connectFixture(controller, control);
    controller.start(1, 1, {2, 1.5});
    EXPECT_EQ(writes.size(), 2); // Only the two explicitly permitted information queries.
    clock = 400; control.receive(motorStatus(4, 0, 0)); controller.advance(clock);
    controller.start(1, 2, {2, 1.5}); control.pump(clock); control.receive(wire("5301"));
    control.pump(600);
    ASSERT_EQ(writes.size(), 4);
    EXPECT_EQ(writes.last().toHex(), "53021401");
    controller.stop(2, 3);
    EXPECT_EQ(writes.last().toHex(), "5303");
}
TEST(FitShowAdapter, PreservesRawStopSpeedAndDoesNotRefreshFromAckOrCache) {
    FitShow::Control control;
    qint64 clock = 0;
    FitShowController controller(control, syntheticProfile(), [&]() { return clock; },
                                [&]() { control.setTransport(true); }, [&]() { control.setTransport(false); });
    Telemetry latest;
    int samples = 0;
    QObject::connect(&controller, &Controller::sample, [&](Telemetry sample) { latest = sample; ++samples; });
    connectFixture(controller, control);
    clock = 400; control.receive(motorStatus(4, 5, 1)); controller.advance(clock);
    EXPECT_EQ(samples, 1);
    EXPECT_FALSE(latest.running);
    EXPECT_DOUBLE_EQ(latest.actual.speedKmh, 0.5);
    EXPECT_DOUBLE_EQ(latest.actual.gradePercent, 1.5);
    clock = 600; control.receive(wire("5303")); controller.advance(clock);
    clock = 800; controller.advance(clock);
    EXPECT_EQ(samples, 1);
    EXPECT_EQ(latest.atMs, 400);
}
TEST(FitShowAdapter, MissingFirmwareAndChangedBoundsRejectMotorCommands) {
    for (bool wrongIdentity : {false, true}) {
        FitShow::Control control;
        qint64 clock = 0;
        FitShowController controller(control, syntheticProfile(), [&]() { return clock; },
                                    [&]() { control.setTransport(true); }, [&]() { control.setTransport(false); });
        QList<QByteArray> writes;
        QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray payload, quint64, quint64) { writes.append(payload); });
        connectFixture(controller, control);
        if (wrongIdentity) controller.observeIdentity("test-model", "different-firmware");
        else control.receive(wire("50025005"));
        clock = 400; control.receive(motorStatus(4, 0, 0)); controller.advance(clock);
        controller.start(1, 1, {2, 0}); control.pump(clock);
        for (const auto &payload : writes) EXPECT_NE(uint8_t(payload[0]), 0x53);
    }
}
TEST(FitShowAdapter, UnmappedGradeAndDelayedTelemetryFaultWithoutInventingPosition) {
    FitShow::Control control;
    qint64 clock = 0;
    FitShowController controller(control, syntheticProfile(), [&]() { return clock; },
                                [&]() { control.setTransport(true); }, [&]() { control.setTransport(false); });
    int faults = 0, samples = 0;
    QObject::connect(&controller, &Controller::fault, [&](QString) { ++faults; });
    QObject::connect(&controller, &Controller::sample, [&](Telemetry) { ++samples; });
    connectFixture(controller, control);
    clock = 400; control.receive(motorStatus(4, 0, 8)); controller.advance(clock);
    EXPECT_EQ(faults, 1); EXPECT_EQ(samples, 0);
    control.receive(motorStatus(4, 0, 0)); clock = 3000; controller.advance(clock);
    EXPECT_EQ(faults, 2); EXPECT_EQ(samples, 0);
}
TEST(FitShowControl, StopStillWinsAfterSeveralFaultInvalidations) {
    FitShow::Control control;
    QList<QByteArray> writes;
    control.setTransport(true); control.allowDiagnostics(true); reportedLimits(control);
    ASSERT_TRUE(control.arm(1));
    QObject::connect(&control, &FitShow::Control::payloadReady, [&](QByteArray payload, quint64, quint64) { writes.append(payload); });
    control.target(1, 1, 20, 1); control.pump(200);
    control.cancel(); control.cancel();
    control.stop(2, 2);
    ASSERT_EQ(writes.size(), 2);
    EXPECT_EQ(writes.last().toHex(), "5303");
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
