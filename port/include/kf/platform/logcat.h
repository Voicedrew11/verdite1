#ifndef KF_PLATFORM_LOGCAT_H
#define KF_PLATFORM_LOGCAT_H

namespace kf {
// On Android, stdout and stderr lead nowhere: this sends what the game prints to logcat,
// line by line, under the tag "Verdite1" (adb logcat -s Verdite1). Called first in main.
// Elsewhere it does nothing. False when the output could not be redirected.
bool logcat_start();
}

#endif // KF_PLATFORM_LOGCAT_H
