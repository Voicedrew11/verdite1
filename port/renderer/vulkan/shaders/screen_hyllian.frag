#version 450
#extension GL_GOOGLE_include_directive : require
// Hyllian's CRT, second pass (crt-hyllian-pass1): the filtered lines drawn as
// beams that widen with brightness, then the phosphor mask in window pixels.
// The mask and corner code below is Hyllian's as published (its mask table
// from hunterk's subpixel_masks.h), unchanged but for the macros it reads.
#include "screen.glsl"
#include "hyllian.glsl"

const float beam_min_width = 0.72;   // BEAM_MIN_WIDTH
const float beam_max_width = 1.0;    // BEAM_MAX_WIDTH
const float scanlines_shape = 1.0;   // SCANLINES_SHAPE: 1 soft (Gaussian)
const float mask_strength = 1.0;     // MASK_STRENGTH
const float mask_gamma = 2.4;        // H_MaskGamma
#define CORNER_SIZE 0.02
#define CORNER_SMOOTHNESS (80.0 * pow(1.10, 10.0))

const vec2 corner_aspect   = vec2(1.0,  0.75);

float corner(vec2 coord)
{
    coord = (coord - vec2(0.5)) + vec2(0.5, 0.5);
    coord = min(coord, vec2(1.0) - coord) * corner_aspect;
    vec2 cdist = vec2(CORNER_SIZE);
    coord = (cdist - min(coord, cdist));
    float dist = sqrt(dot(coord, coord));
    
    return clamp((cdist.x - dist)*CORNER_SMOOTHNESS, 0.0, 1.0);
}

// The curve is hyllian_warp (hyllian.glsl).

// Shadow mask.
vec3 Mask(vec2 pos, float shadowMask)
{
    const float maskDark  = 0.0;
    const float maskLight = 1.0;

    vec3 mask = vec3(maskDark, maskDark, maskDark);
  
    // Very compressed TV style shadow mask.
    if (shadowMask == 15.0) 
    {
        float line = maskLight;
        float odd = 0.0;
        
        if (fract(pos.x*0.166666666) < 0.5) odd = 1.0;
        if (fract((pos.y + odd) * 0.5) < 0.5) line = maskDark;  
        
        pos.x = fract(pos.x*0.333333333);

        if      (pos.x < 0.333) mask.b = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.r = maskLight;
        mask*=line;  
    } 

    // Aperture-grille. This mask is the same as mask 2.
/*    else if (shadowMask == 16.0) 
    {
        pos.x = fract(pos.x*0.333333333);

        if      (pos.x < 0.333) mask.b = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.r = maskLight;
    } 
*/
    // Stretched VGA style shadow mask (same as prior shaders).
    else if (shadowMask == 16.0) 
    {
        pos.x += pos.y*3.0;
        pos.x  = fract(pos.x*0.166666666);

        if      (pos.x < 0.333) mask.b = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.r = maskLight;
    }

    // VGA style shadow mask.
    else if (shadowMask == 17.0) 
    {
        pos.xy  = floor(pos.xy*vec2(1.0, 0.5));
        pos.x  += pos.y*3.0;
        pos.x   = fract(pos.x*0.166666666);

        if      (pos.x < 0.333) mask.b = maskLight;
        else if (pos.x < 0.666) mask.g = maskLight;
        else                    mask.r = maskLight;
    }

    return mask;
}


/* Mask code pasted from subpixel_masks.h. Masks 3 and 4 added. */
vec3 mask_weights(vec2 coord, float phosphor_layout){
   
//   if (lottes_mask > 0.5) return Mask(coord);

   if (phosphor_layout > 14.0) return Mask(coord, phosphor_layout);

   vec3 weights = vec3(1.,1.,1.);

   const float on  = 1.;
   const float off = 0.;

   const vec3 red     = vec3(off, off, on );
   const vec3 green   = vec3(off, on,  off);
   const vec3 blue    = vec3(on,  off, off);
   const vec3 magenta = vec3(on,  off, on );
   const vec3 yellow  = vec3(off, on,  on );
   const vec3 cyan    = vec3(on,  on,  off);
   const vec3 black   = vec3(off, off, off);
   const vec3 white   = vec3(on,  on,  on );

   int w, z = 0;
   
   // This pattern is used by a few layouts, so we'll define it here
   vec3 aperture_classic = mix(magenta, green, floor(mod(coord.x, 2.0)));
   
   if(phosphor_layout == 0.) return weights;

   else if(phosphor_layout == 1.){
      // classic aperture for RGB panels; good for 1080p, too small for 4K+
      // aka aperture_1_2_bgr
      weights  = aperture_classic;
      return weights;
   }

   else if(phosphor_layout == 2.){
      // Classic RGB layout; good for 1080p and lower
      const vec3 aperture1[3] = vec3[](red, green, blue);
//      vec3 bw3[3] = vec3[](black, yellow, blue);
      
      z = int(floor(mod(coord.x, 3.0)));
      
      weights = aperture1[z];
      return weights;
   }

   else if(phosphor_layout == 3.){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 1080p and lower
      const vec3 aperture2[3] = vec3[](black, white, black);
      
      z = int(floor(mod(coord.x, 3.0)));
      
      weights = aperture2[z];
      return weights;
   }

   else if(phosphor_layout == 4.){
      // reduced TVL aperture for RGB panels. Good for 4k.
      // aperture_2_4_rgb
      
      const vec3 aperture3[4] = vec3[](red, yellow, cyan, blue);
      
      w = int(floor(mod(coord.x, 4.0)));
      
      weights = aperture3[w];
      return weights;
   }
   

   else if(phosphor_layout == 5.){
      // black and white aperture; good for weird subpixel layouts and low brightness; good for 4k 
      const vec3 aperture4[4] = vec3[](black, black, white, white);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = aperture4[z];
      return weights;
   }


   else if(phosphor_layout == 6.){
      // aperture_1_4_rgb; good for simulating lower 
      const vec3 aperture5[4] = vec3[](red, green, blue, black);
      
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = aperture5[z];
      return weights;
   }

   else if(phosphor_layout == 7.){
      // 2x2 shadow mask for RGB panels; good for 1080p, too small for 4K+
      // aka delta_1_2x1_bgr
      vec3 inverse_aperture = mix(green, magenta, floor(mod(coord.x, 2.0)));
      weights               = mix(aperture_classic, inverse_aperture, floor(mod(coord.y, 2.0)));
      return weights;
   }

   else if(phosphor_layout == 8.){
      // delta_2_4x1_rgb
      const vec3 delta1[2][4] = {
         {red, yellow, cyan, blue},
         {cyan, blue, red, yellow}
      };
      
      w = int(floor(mod(coord.y, 2.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta1[w][z];
      return weights;
   }

   else if(phosphor_layout == 9.){
      // delta_1_4x1_rgb; dunno why this is called 4x1 when it's obviously 4x2 /shrug
      const vec3 delta1[2][4] = {
         {red,  green, blue, black},
         {blue, black, red,  green}
      };
      
      w = int(floor(mod(coord.y, 2.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta1[w][z];
      return weights;
   }
   
   else if(phosphor_layout == 10.){
      // delta_2_4x2_rgb
      const vec3 delta[4][4] = {
         {red,  yellow, cyan, blue},
         {red,  yellow, cyan, blue},
         {cyan, blue,   red,  yellow},
         {cyan, blue,   red,  yellow}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 4.0)));
      
      weights = delta[w][z];
      return weights;
   }

   else if(phosphor_layout == 11.){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      const vec3 slotmask[4][6] = {
         {red, green, blue,    red, green, blue,},
         {red, green, blue,  black, black, black},
         {red, green, blue,    red, green, blue,},
         {black, black, black, red, green, blue,}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 6.0)));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[w][z];
      return weights;
   }

   else if(phosphor_layout == 12.){
      // slot mask for RGB panels; looks okay at 1080p, looks better at 4K
      const vec3 slotmask[4][6] = {
         {black,  white, black,   black,  white, black,},
         {black,  white, black,  black, black, black},
         {black,  white, black,  black,  white, black,},
         {black, black, black,  black,  white, black,}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 6.0)));

      // use the indexes to find which color to apply to the current pixel
      weights = slotmask[w][z];
      return weights;
   }

   else if(phosphor_layout == 13.){
      // based on MajorPainInTheCactus' HDR slot mask
      const vec3 slot[4][8] = {
         {red,   green, blue,  black, red,   green, blue,  black},
         {red,   green, blue,  black, black, black, black, black},
         {red,   green, blue,  black, red,   green, blue,  black},
         {black, black, black, black, red,   green, blue,  black}
      };
      
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 8.0)));
      
      weights = slot[w][z];
      return weights;
   }

   else if(phosphor_layout == 14.){
      // same as above but for RGB panels
      const vec3 slot2[4][10] = {
         {red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue },
         {black, green,  green, blue,  blue,  red,   red,    black, black, black},
         {red,   yellow, green, blue,  blue,  red,   yellow, green, blue,  blue },
         {red,   red,    black, black, black, black, green,  green, blue,  blue }
      };
   
      w = int(floor(mod(coord.y, 4.0)));
      z = int(floor(mod(coord.x, 10.0)));
      
      weights = slot2[w][z];
      return weights;
   }
   
   else return weights;
}




#define pi 3.1415926535897932384626433832795
#define wa (0.5 * pi)
#define wb (pi)

vec3 resampler3(vec3 x) {
    vec3 res;
    res.x = (x.x <= 0.001) ? 1.0 : sin(x.x * wa) * sin(x.x * wb) / (wa * wb * x.x * x.x);
    res.y = (x.y <= 0.001) ? 1.0 : sin(x.y * wa) * sin(x.y * wb) / (wa * wb * x.y * x.y);
    res.z = (x.z <= 0.001) ? 1.0 : sin(x.z * wa) * sin(x.z * wb) / (wa * wb * x.z * x.z);
    return res;
}

vec3 get_scanlines(vec3 d0, vec3 d1, vec3 color0, vec3 color1) {
    if (scanlines_shape > 0.5) {
        d0 = exp(-16.0 * d0 * d0);
        d1 = exp(-16.0 * d1 * d1);
    } else {
        d0 = resampler3(clamp(2.0 * d0, 0.0, 1.0));
        d1 = resampler3(clamp(2.0 * d1, 0.0, 1.0));
    }
    return color0 * d0 + color1 * d1;
}

// A filtered line's colour at a column of the lines image, the row held to its edge.
vec3 line_color(int column, int row) {
    return picture_texel(ivec2(column, clamp(row, 0, int(screen.grid.y) - 1)));
}

void main() {
    vec2 size = vec2(textureSize(picture, 0));   // the letterbox's width by the game's lines
    vec2 uv = screen_uv();
    if (screen.curvature > 0.5)
        uv = hyllian_warp(uv, vec2(0.0, warp_y) * 15.0);

    vec2 pix = uv * size + vec2(0.0, -0.5);
    int column = int(floor(pix.x));
    int row = int(floor(pix.y));
    float fy = fract(pix.y);

    vec3 color0 = line_color(column, row);
    vec3 color1 = line_color(column, row + 1);

    // SCANLINES_STRENGTH as Hyllian's macro gives it: the soft shape takes 0.16 off.
    float strength = -0.16 * scanlines_shape + screen.scanlines;
    vec3 lum0 = mix(vec3(beam_min_width), vec3(beam_max_width), color0);
    vec3 lum1 = mix(vec3(beam_min_width), vec3(beam_max_width), color1);
    vec3 d0 = strength * fy / (lum0 * lum0 + 0.00001);
    vec3 d1 = strength * (1.0 - fy) / (lum1 * lum1 + 0.00001);

    vec3 color = screen.brightness * get_scanlines(d0, d1, color0, color1);
    color = pow(clamp(color, 0.0, 1.0), vec3(1.0 / output_gamma));

    vec3 mask = mask_weights(gl_FragCoord.xy - screen.rect.xy, screen.mask_layout);
    color = mask + (1.0 - 2.0 * mask) * pow(abs(mask - color), mask * mask_strength * (mask_gamma - 1.0) + 1.0);

    if (screen.curvature > 0.5)
        color *= corner(uv);
    out_color = vec4(color, 1.0);
}
