#pragma once
#include "controller.h"
#include "fitshowcontrol.h"
#include <QString>
namespace Treadmill {
struct InclinePoint { int code; double percent; };
struct DeviceProfile {
    QString address, name, model, firmware, evidence;
    int service = 0, write = 0, notify = 0;
    enum class SpeedUnit { Unknown, Kmh, Mph };
    SpeedUnit speedUnit = SpeedUnit::Unknown;
    int minSpeedCode = 0, maxSpeedCode = 0, runningMinCode = 0, speedStepCode = 0, unitTag = -1;
    QVector<InclinePoint> inclines;
    double maxGradeMappingLoss = 0;
    bool diagnosticsConfirmed = false;
    bool controlsVerified = false, speedFeedbackVerified = false, inclineFeedbackVerified = false, stopFeedbackVerified = false;
    bool pauseVerified = false;
    double speedScale() const { return speedUnit == SpeedUnit::Mph ? 1.60934 : (speedUnit == SpeedUnit::Kmh ? 1.0 : NAN); }
    bool motorEvidenceComplete() const;
    QString validate() const;
    Capabilities capabilities() const;
    bool matches(const FitShow::Limits &limits) const;
    bool toWire(Target target, int &speed, int &incline) const;
    bool fromWire(int speed, int incline, Target &target) const;
    bool map(Target requested, Target &mapped) const;
};
struct ProfileLoadResult { DeviceProfile profile; QString error; };
ProfileLoadResult loadProfile(const QString &path);
}
