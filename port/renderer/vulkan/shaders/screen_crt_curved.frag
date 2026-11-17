#version 450
#extension GL_GOOGLE_include_directive : require
// Timothy Lottes' CRT on a curved tube, a shadow mask.
#define CRT_CURVED 1
#include "screen.glsl"
#include "crt_lottes.glsl"
