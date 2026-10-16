#include "motorgate.h"
#include <gtest/gtest.h>
using Treadmill::MotorGate;
using Treadmill::Profile;

namespace {
const QString kStart = "02530100000000000000005203";
const QString kStop = "0253035003";

QByteArray frame(const QByteArray &payload) {
    quint8 checksum = 0;
    for (const char c : payload) checksum ^= quint8(c);
    return QByteArray(1, 2) + payload + char(checksum) + char(3);
}
QString hex(const QByteArray &bytes) { return QString::fromLatin1(bytes.toHex()); }
QString target(int speed, int incline) { return hex(frame(QByteArray("\x53\x02", 2) + char(speed) + char(incline))); }
QByteArray limits() { return frame(QByteArray::fromHex("50023c0a00")); }
QByteArray inclineLimits() { return frame(QByteArray::fromHex("50030800")); }
QByteArray normal() { return frame(QByteArray::fromHex("5100")); }
QByteArray status(int state, int speed, int incline) {
    return frame(QByteArray("\x51", 1) + char(state) + char(speed) + char(incline) + QByteArray(9, '\0'));
}

// Reviewed limits plus three fresh NORMAL reports, as the preflight requires.
void preflight(MotorGate &gate, qint64 now = 0) {
    gate.observe(limits(), now);
    gate.observe(inclineLimits(), now);
    for (int i = 0; i < 3; ++i) gate.observe(normal(), now);
}
}

TEST(MotorGate, StartRefusedUntilLimitsAndThreeNormalReports) {
    MotorGate gate("start-stop", "", 12);
    EXPECT_THROW(gate.approve(kStart, 0), std::invalid_argument);
    gate.observe(limits(), 0);
    gate.observe(inclineLimits(), 0);
    gate.observe(normal(), 0);
    gate.observe(normal(), 0);
    EXPECT_THROW(gate.approve(kStart, 0), std::invalid_argument);
    gate.observe(normal(), 0);
    EXPECT_NO_THROW(gate.approve(kStart, 0));
    EXPECT_TRUE(gate.started());
}

TEST(MotorGate, StartRefusedWithStaleStatusOrDifferentLimits) {
    MotorGate stale("start-stop", "", 12);
    preflight(stale, 0);
    EXPECT_THROW(stale.approve(kStart, 1501), std::invalid_argument);
    MotorGate other("start-stop", "", 12);
    other.observe(frame(QByteArray::fromHex("50027800a0")), 0); // 10/120 with tag 0x20 is refused for motors
    other.observe(inclineLimits(), 0);
    for (int i = 0; i < 3; ++i) other.observe(normal(), 0);
    EXPECT_THROW(other.approve(kStart, 0), std::invalid_argument);
}

TEST(MotorGate, SleepAndSafetyStatusesNeverCountAsNormal) {
    MotorGate gate("start-stop", "", 12);
    gate.observe(limits(), 0);
    gate.observe(inclineLimits(), 0);
    for (int i = 0; i < 3; ++i) gate.observe(frame(QByteArray::fromHex("510602")), 0); // sleep 06/02
    EXPECT_THROW(gate.approve(kStart, 0), std::invalid_argument);
}

TEST(MotorGate, FramesOutsideTheAllowlistAreRefused) {
    MotorGate gate("start-stop", "", 12);
    preflight(gate);
    gate.approve(kStart, 0);
    EXPECT_THROW(gate.approve(target(60, 8), 0), std::invalid_argument);
    EXPECT_THROW(gate.approve(hex(frame(QByteArray::fromHex("5306"))), 0), std::invalid_argument); // pause
    EXPECT_THROW(gate.approve("0253010000000080000000d203", 0), std::invalid_argument); // retired READY
    EXPECT_THROW(gate.approve("0253020a005b04", 0), std::invalid_argument); // bad envelope
    EXPECT_THROW(gate.approve("0253020a005c03", 0), std::invalid_argument); // bad checksum
    EXPECT_NO_THROW(gate.approve(target(10, 0), 0));
}

TEST(MotorGate, QueriesAreAlwaysAllowedAndAcceptUppercase) {
    MotorGate gate("start-stop", "", 12);
    EXPECT_NO_THROW(gate.approve("0250025203", 0));
    EXPECT_NO_THROW(gate.approve("0250035303", 0));
    EXPECT_NO_THROW(gate.approve("02515103", 0));
    EXPECT_NO_THROW(gate.approve("0250025203", 0));
    MotorGate upper("start-stop", "", 12);
    EXPECT_NO_THROW(upper.approve("0250035303", 0));
}

TEST(MotorGate, StopOnlyAfterStartAndOnlyOnce) {
    MotorGate gate("start-stop", "", 12);
    EXPECT_THROW(gate.approve(kStop, 0), std::invalid_argument);
    preflight(gate);
    gate.approve(kStart, 0);
    EXPECT_NO_THROW(gate.approve(kStop, 0));
    EXPECT_TRUE(gate.stopAttempted());
    EXPECT_THROW(gate.approve(kStop, 0), std::invalid_argument);
    EXPECT_FALSE(gate.needsStop(100000)); // already stopped: no second emergency stop
}

TEST(MotorGate, StartIsAvailableOnceAndTargetsAfterStopAreRefused) {
    MotorGate gate("start-stop", "", 12);
    preflight(gate);
    gate.approve(kStart, 0);
    EXPECT_THROW(gate.approve(kStart, 0), std::invalid_argument);
    gate.approve(kStop, 0);
    EXPECT_THROW(gate.approve(target(10, 0), 0), std::invalid_argument);
}

TEST(MotorGate, WatchdogRequestsStopOnStaleStatusDeadlineAndUnsafeStates) {
    {
        MotorGate gate("start-stop", "", 12);
        preflight(gate);
        gate.approve(kStart, 0);
        gate.observe(status(3, 10, 0), 100);
        EXPECT_FALSE(gate.needsStop(1500));
        EXPECT_TRUE(gate.needsStop(1601)); // status older than 1.5 s
    }
    {
        MotorGate gate("start-stop", "", 12);
        preflight(gate);
        gate.approve(kStart, 0);
        gate.observe(status(3, 10, 0), 11990);
        EXPECT_TRUE(gate.needsStop(12000)); // 12 s trial deadline
    }
    for (const int state : {1, 5, 6, 7, 10}) {
        MotorGate gate("start-stop", "", 12);
        preflight(gate);
        gate.approve(kStart, 0);
        gate.observe(state == 5 || state == 6 || state == 7 ? frame(QByteArray("\x51", 1) + char(state) + char(2)) : status(state, 10, 0), 100);
        EXPECT_TRUE(gate.needsStop(101)) << "state " << state;
    }
    MotorGate quiet("start-stop", "", 12);
    EXPECT_FALSE(quiet.needsStop(1000000)); // no watchdog before a start is attempted
}

TEST(MotorGate, WatchdogRequestsStopWhenSpeedOrInclineExceedsTheCeilingOrBoundsChange) {
    MotorGate speed("start-stop", "", 12);
    preflight(speed);
    speed.approve(kStart, 0);
    speed.observe(status(3, 11, 0), 100);
    EXPECT_TRUE(speed.needsStop(101));
    MotorGate incline("start-stop", "", 12);
    preflight(incline);
    incline.approve(kStart, 0);
    incline.observe(status(3, 10, 1), 100);
    EXPECT_TRUE(incline.needsStop(101));
    MotorGate bounds("start-stop", "", 12);
    preflight(bounds);
    bounds.approve(kStart, 0);
    bounds.observe(frame(QByteArray::fromHex("50023c0a01")), 50); // unit tag changed mid-run
    bounds.observe(status(3, 10, 0), 100);
    EXPECT_TRUE(bounds.needsStop(101));
}

TEST(MotorGate, InvalidFramesDoNotRefreshTheStatusClock) {
    MotorGate gate("start-stop", "", 12);
    preflight(gate);
    gate.approve(kStart, 0);
    gate.observe(status(3, 10, 0), 100);
    QByteArray corrupted = status(3, 10, 0);
    corrupted[corrupted.size() - 2] = char(corrupted[corrupted.size() - 2] ^ 0x40);
    gate.observe(corrupted, 1200);
    gate.observe(frame(QByteArray("\x51\x03\x0a\x00", 4)), 1300); // motor data shorter than 16 bytes
    EXPECT_TRUE(gate.needsStop(1700));
}

TEST(MotorGate, WorkoutTargetsMustFollowTheReviewedSequenceInOrder) {
    MotorGate gate("workout", "20:1,30:2,10:0", 600);
    preflight(gate);
    gate.approve(kStart, 0);
    EXPECT_THROW(gate.approve(target(30, 2), 0), std::invalid_argument); // out of order
    EXPECT_NO_THROW(gate.approve(target(20, 1), 0));
    EXPECT_THROW(gate.approve(target(20, 1), 0), std::invalid_argument); // not repeatable
    EXPECT_NO_THROW(gate.approve(target(30, 2), 0));
    EXPECT_NO_THROW(gate.approve(target(10, 0), 0));
    EXPECT_THROW(gate.approve(target(10, 0), 0), std::invalid_argument); // sequence exhausted
}

TEST(MotorGate, WorkoutCeilingsComeFromThePlan) {
    MotorGate gate("workout", "20:1,30:2", 600);
    preflight(gate);
    gate.approve(kStart, 0);
    gate.observe(status(3, 30, 2), 100);
    EXPECT_FALSE(gate.needsStop(101));
    gate.observe(status(3, 31, 2), 200);
    EXPECT_TRUE(gate.needsStop(201));
}

TEST(MotorGate, WorkoutUsesItsOwnDeadline) {
    MotorGate gate("workout", "20:1", 600);
    preflight(gate);
    gate.approve(kStart, 0);
    gate.observe(status(3, 10, 0), 599000);
    EXPECT_FALSE(gate.needsStop(599500));
    gate.observe(status(3, 10, 0), 599900);
    EXPECT_TRUE(gate.needsStop(600000));
}

TEST(MotorGate, ConstructorRejectsUnsupportedOrMalformedPlans) {
    EXPECT_THROW(MotorGate("pause", "", 18), std::invalid_argument);
    EXPECT_THROW(MotorGate("ready-idle", "", 30), std::invalid_argument);
    EXPECT_THROW(MotorGate("incline-timing-4", "", 150), std::invalid_argument);
    EXPECT_THROW(MotorGate("speed-70", "", 60), std::invalid_argument);
    EXPECT_THROW(MotorGate("incline-9", "", 80), std::invalid_argument);
    EXPECT_THROW(MotorGate("nonsense", "", 12), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "", 600), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "20:1", 9001), std::invalid_argument);
    EXPECT_NO_THROW(MotorGate("workout", "20:1", 9000));
    EXPECT_THROW(MotorGate("workout", "9:0", 600), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "61:0", 600), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "20:9", 600), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "20:1,20:1", 600), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "20", 600), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "a:b", 600), std::invalid_argument);
    EXPECT_NO_THROW(MotorGate("speed-60", "", 60));
    EXPECT_NO_THROW(MotorGate("incline-8", "", 80));
}

TEST(MotorGate, SpeedAndInclineTrialsOnlyAllowTheirOwnTargets) {
    MotorGate gate("speed-30", "", 60);
    preflight(gate);
    gate.approve(kStart, 0);
    EXPECT_THROW(gate.approve(target(40, 0), 0), std::invalid_argument);
    EXPECT_THROW(gate.approve(target(30, 0), 0), std::invalid_argument); // must return to 10/0 first, in order
    EXPECT_NO_THROW(gate.approve(target(10, 0), 0));
    EXPECT_NO_THROW(gate.approve(target(30, 0), 0));
    EXPECT_NO_THROW(gate.approve(target(10, 0), 0));
}

namespace {
QByteArray runningLimits() { return frame(QByteArray::fromHex("50027" "80a20")); } // max 120, min 10, unit tag 0x20
QByteArray ftmsFrame(double kmh) {
    const int hundredths = int(kmh * 100 + 0.5);
    QByteArray bytes = QByteArray::fromHex("8c05") + char(hundredths & 0xff) + char(hundredths >> 8) + QByteArray(15, '\0');
    return bytes;
}
void runningPreflight(MotorGate &gate, qint64 now = 0) {
    gate.observe(runningLimits(), now);
    gate.observe(inclineLimits(), now);
    for (int i = 0; i < 3; ++i) gate.observe(normal(), now);
}
// A started 12 km/h-ceiling workout in the running profile.
MotorGate startedRunning(const QString &sequence = "80:0,120:2") {
    MotorGate gate("workout", sequence, 600, Profile::Running);
    runningPreflight(gate);
    gate.approve(kStart, 0);
    return gate;
}
}

TEST(MotorGateRunning, OnlyTheRunningLimitsStartARunningWorkoutAndOnlyWalkingLimitsStartAWalkingOne) {
    MotorGate running("workout", "80:0", 600, Profile::Running);
    preflight(running); // walking-mode limits: 10-60, unit 0
    EXPECT_THROW(running.approve(kStart, 0), std::invalid_argument);
    runningPreflight(running);
    EXPECT_NO_THROW(running.approve(kStart, 0));
    MotorGate walking("workout", "50:0", 600);
    runningPreflight(walking); // running-mode limits must never start a walking plan
    EXPECT_THROW(walking.approve(kStart, 0), std::invalid_argument);
}

TEST(MotorGateRunning, ChangedLimitsAfterTheStartStopTheRun) {
    MotorGate gate = startedRunning();
    gate.observe(runningLimits(), 100);
    gate.observe(status(3, 80, 0), 100);
    gate.observeFtms(ftmsFrame(8.0), 100);
    EXPECT_FALSE(gate.needsStop(101));
    gate.observe(limits(), 200); // the console switched back to walking limits mid-run
    gate.observe(status(3, 80, 0), 200);
    EXPECT_TRUE(gate.needsStop(201));
    EXPECT_EQ(gate.stopReason(), "Limits changed during the run");
}

TEST(MotorGateRunning, SpeedLimitsFollowTheProfile) {
    EXPECT_NO_THROW(MotorGate("workout", "120:8", 600, Profile::Running));
    EXPECT_THROW(MotorGate("workout", "121:0", 600, Profile::Running), std::invalid_argument);
    EXPECT_THROW(MotorGate("workout", "70:0", 600), std::invalid_argument); // walking profile stays at raw 60
    EXPECT_NO_THROW(MotorGate("workout", "60:8", 600));
    EXPECT_THROW(MotorGate("speed-30", "", 60, Profile::Running), std::invalid_argument);
    EXPECT_THROW(MotorGate("start-stop", "", 12, Profile::Running), std::invalid_argument);
}

TEST(MotorGateRunning, RunningTargetsAreAllowedInOrderAndAboveTheCeilingStops) {
    MotorGate gate = startedRunning("80:0,120:2");
    EXPECT_NO_THROW(gate.approve(target(80, 0), 0));
    EXPECT_NO_THROW(gate.approve(target(120, 2), 0));
    gate.observe(status(3, 120, 2), 100);
    gate.observeFtms(ftmsFrame(12.0), 100);
    EXPECT_FALSE(gate.needsStop(101));
    gate.observe(status(3, 121, 2), 200);
    EXPECT_TRUE(gate.needsStop(201));
}

TEST(MotorGateRunning, FtmsAgreementIsRequiredAboveSixKmh) {
    {   // agreeing speeds: fine
        MotorGate gate = startedRunning();
        for (int i = 0; i < 10; ++i) { gate.observeFtms(ftmsFrame(8.1), 100 + i * 100); gate.observe(status(3, 80, 0), 100 + i * 100); }
        EXPECT_FALSE(gate.needsStop(1000));
    }
    {   // four consecutive disagreeing status reports: stop, with the reason
        MotorGate gate = startedRunning();
        gate.observeFtms(ftmsFrame(6.5), 100);
        for (int i = 0; i < 3; ++i) { gate.observe(status(3, 80, 0), 100 + i * 100); EXPECT_FALSE(gate.needsStop(101 + i * 100)); }
        gate.observe(status(3, 80, 0), 400);
        EXPECT_TRUE(gate.needsStop(401));
        EXPECT_TRUE(gate.stopReason().contains("disagrees"));
    }
    {   // a transient mismatch that clears does not accumulate
        MotorGate gate = startedRunning();
        for (int round = 0; round < 3; ++round) {
            gate.observeFtms(ftmsFrame(6.5), 100 + round * 1000);
            for (int i = 0; i < 3; ++i) gate.observe(status(3, 80, 0), 100 + round * 1000 + i);
            gate.observeFtms(ftmsFrame(8.0), 500 + round * 1000);
            gate.observe(status(3, 80, 0), 600 + round * 1000);
        }
        EXPECT_FALSE(gate.needsStop(3100));
    }
    {   // no FTMS speed at all above 6 km/h: tolerated for a moment (the streams are unordered), then stopped
        MotorGate gate = startedRunning();
        gate.observe(status(3, 80, 0), 100);
        EXPECT_FALSE(gate.needsStop(101));
        gate.observe(status(3, 80, 0), 1500);
        EXPECT_FALSE(gate.needsStop(1501));
        gate.observe(status(3, 80, 0), 2200);
        EXPECT_TRUE(gate.needsStop(2201));
        EXPECT_TRUE(gate.stopReason().contains("cannot be observed"));
    }
    {   // an FTMS sample arriving just after the first status clears the doubt
        MotorGate gate = startedRunning();
        gate.observe(status(3, 80, 0), 100);
        gate.observeFtms(ftmsFrame(8.0), 600);
        for (int t = 700; t <= 5000; t += 500) { gate.observeFtms(ftmsFrame(8.0), t); gate.observe(status(3, 80, 0), t); }
        EXPECT_FALSE(gate.needsStop(5001));
    }
    {   // FTMS went silent: stale after 3 s, stop 2 s later
        MotorGate gate = startedRunning();
        gate.observeFtms(ftmsFrame(8.0), 100);
        gate.observe(status(3, 80, 0), 3000);
        EXPECT_FALSE(gate.needsStop(3050));
        gate.observe(status(3, 80, 0), 3200); // FTMS is 3.1 s old: the doubt starts here
        EXPECT_FALSE(gate.needsStop(3201));
        gate.observe(status(3, 80, 0), 5300);
        EXPECT_TRUE(gate.needsStop(5301));
    }
    {   // up to 6 km/h the check does not apply, even without FTMS
        MotorGate gate = startedRunning("50:0,80:0");
        gate.observe(status(3, 50, 0), 100);
        gate.observe(status(3, 60, 0), 200);
        EXPECT_FALSE(gate.needsStop(250));
    }
    {   // a "more data" frame carries no speed and must not refresh the clock
        MotorGate gate = startedRunning();
        gate.observeFtms(ftmsFrame(8.0), 100);
        QByteArray more = ftmsFrame(8.0);
        more[0] = char(0x8d);
        gate.observeFtms(more, 2000);
        gate.observe(status(3, 80, 0), 3500); // the last real speed sample is 3.4 s old: the doubt starts
        EXPECT_FALSE(gate.needsStop(3501));
        gate.observeFtms(more, 5000);
        gate.observe(status(3, 80, 0), 5600);
        EXPECT_TRUE(gate.needsStop(5601));
    }
    {   // the walking profile ignores FTMS entirely
        MotorGate gate("workout", "60:0", 600);
        preflight(gate);
        gate.approve(kStart, 0);
        gate.observeFtms(ftmsFrame(2.0), 100);
        gate.observe(status(3, 60, 0), 100);
        gate.observe(status(3, 60, 0), 200);
        EXPECT_FALSE(gate.needsStop(250));
    }
}
