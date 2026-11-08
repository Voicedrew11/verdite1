#ifndef KF_PLATFORM_IOS_H
#define KF_PLATFORM_IOS_H
#include <string>
namespace kf {
// Imports a BIN/CUE pair (or a single BIN/ISO/CHD) into Documents/Import.
// A disc is chosen before a patch, and a CUE before its BIN.
std::string ios_choose_disc();
// Removes Documents/Import once its files have been read, so a disc is not kept twice.
void ios_discard_imports();
void ios_touch_feedback();
}
#endif
