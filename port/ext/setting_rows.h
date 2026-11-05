#pragma once
// One decision a player makes, kept as several saved settings: what a combined row of the option
// table reads and writes. Each pair reads the choice from the settings and sets the settings for it.
#include "settings_values.h"

#include <array>

namespace kf::ext {

enum class ControlStyle { DPad, Tank, Twin, TwinAccelerated };
ControlStyle control_style(const Settings &settings);
void set_control_style(Settings &settings, ControlStyle style);

enum class LookResponse { Eased, StopsDead, Direct };
LookResponse look_response(const Settings &settings);
void set_look_response(Settings &settings, LookResponse response);

enum class DynamicLight { Off, On, Shadows };
DynamicLight dynamic_light(const Settings &settings);
void set_dynamic_light(Settings &settings, DynamicLight light);

// Off, or on at a quality; off keeps the quality for later.
enum class AoChoice { Off, Low, Medium, High };
AoChoice ao_choice(const Settings &settings);
void set_ao_choice(Settings &settings, AoChoice choice);

// 0: no mipmaps. 1: mipmaps alone (trilinear). 2, 4, 8, 16: anisotropic samples over them.
int aniso_choice(const Settings &settings);
void set_aniso_choice(Settings &settings, int choice);

// The enemies' turn: automatic, or a fixed multiple of the original.
inline constexpr double enemy_turn_automatic = 0.0;
double enemy_turn_choice(const Settings &settings);
void set_enemy_turn_choice(Settings &settings, double choice);

// Turn and look speed together. Custom when fine tuning set them apart or off these stops: only
// read, as no row can choose it.
inline constexpr double stick_sens_custom = -1.0;
// Verdite2's stops.
inline constexpr std::array<double, 9> stick_sens_stops = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
double stick_sens(const Settings &settings);
void set_stick_sens(Settings &settings, double sens);

bool wiggle(const Settings &settings);   // the PlayStation's vertex snapping and texture warping
void set_wiggle(Settings &settings, bool on);

// What a file can hold that no row can reach, from before these rows were one: settled once at load.
void settings_tidy(Settings &settings);

} // namespace kf::ext
