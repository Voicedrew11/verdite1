#ifndef KF_PLATFORM_IOS_H
#define KF_PLATFORM_IOS_H
#include <string>
namespace kf {
// Imports a BIN/CUE pair (or a single BIN/ISO/CHD) into Documents/Import.
std::string ios_choose_disc();
void ios_touch_feedback();
}
#endif
