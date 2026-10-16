#pragma once
#include "workout.h"
namespace Treadmill {
// Loads an XML plan or a FIT workout (.fit, see fit/fitworkout.h); the extension decides.
WorkoutLoadResult loadWorkoutFile(const QString &path);
}
