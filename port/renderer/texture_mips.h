#ifndef KF_RENDERER_TEXTURE_MIPS_H
#define KF_RENDERER_TEXTURE_MIPS_H
// A decoded texture page as a mip chain a shader can filter, without knowing a
// graphics API. Coverage is the alpha channel and colour is weighted by it, so
// a texel half covered by a cut-out keeps its colour instead of darkening:
// colour / alpha is the covered texels' average at every level.
//
// Level 0 keeps the decoded categories with STP moved next to opaque:
// transparent alpha 0 (colour 0, as decoded), STP texture_mip_stp_alpha,
// opaque 255. A texel is STP when its alpha is below 255, transparent at 0.
#include <kf/platform/assets.h>

#include <vector>

namespace kf {
inline constexpr u8 texture_mip_stp_alpha = 254;

// page: texture_page_extent square, RGBA8, alpha as texture_decode marks it.
// Returns every level from the page down to 1 x 1, or none if page is not that shape.
std::vector<Image> texture_mip_chain(const Image &page);
} // namespace kf

#endif // KF_RENDERER_TEXTURE_MIPS_H
