// Hyllian's CRT shader (crt-hyllian-pass0 and -pass1 in libretro's
// slang-shaders), ported to Verdite's screen pass. What both passes share.
//
// Copyright (C) 2011-2024 Hyllian - sergiogdb@gmail.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

const float input_gamma = 2.4;    // H_InputGamma
const float output_gamma = 2.2;   // H_OUTPUT_GAMMA
const float warp_x = 0.015;       // WARP_X
const float warp_y = 0.015;       // WARP_Y

#define SQRT_OF_2 1.4142135623730950488016887242097
// 1 - SQRT_OF_2 / 2
#define CONVERGENCE_RADIUS 0.29289321881345247559915563789515

// The tube's curve (torridgristle's), along one axis per pass: distortion is
// (warp_x, 0) for the lines, (0, warp_y) for the window.
vec2 hyllian_warp(vec2 coord, vec2 distortion) {
    vec2 c = coord * 2.0 - 1.0;
    float d = sqrt(c.x * c.x + c.y * c.y);
    c = c / d;
    c = c * (1.0 - pow(vec2(1.0 - (d / SQRT_OF_2)), (1.0 / (1.0 + distortion * 0.2))));
    c = c / (1.0 - pow(vec2(CONVERGENCE_RADIUS), (1.0 / (vec2(1.0) + distortion * 0.2))));
    return c * 0.5 + 0.5;
}
