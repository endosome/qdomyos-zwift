#pragma once
#include "fitshowcontroller.h"
#include "fitshowtreadmill/fitshowtreadmill.h"
#include <QElapsedTimer>
#include <memory>
namespace Treadmill {
// Owns the actual Qt/QZ transport. Workout CLI does not expose hardware execution.
class QzController : public FitShowController {
public:
    explicit QzController(DeviceProfile profile, QObject *parent = nullptr);
    ~QzController() override;
private:
    struct Transport {
        fitshowtreadmill driver {200, true, true, 0, 0, fitshowtreadmill::Mode::Restricted};
        QElapsedTimer clock;
        Transport() { clock.start(); }
    };
    QzController(std::shared_ptr<Transport> transport, DeviceProfile profile, QObject *parent);
    std::shared_ptr<Transport> transport_;
};
}
