#include "motorgate.h"
#include <gtest/gtest.h>
using Treadmill::MotorGate;

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
