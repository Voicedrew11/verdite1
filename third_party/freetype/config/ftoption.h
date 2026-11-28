/* Darker Reality's FreeType options (cmake/rmlui.cmake): FreeType's own, without the compressed font formats,
   whose modules (src/gzip, src/lzw) are left out. Takes the place of include/freetype/config/ftoption.h through
   FT_CONFIG_OPTIONS_H. */
#include <freetype/config/ftoption.h>
#undef FT_CONFIG_OPTION_USE_LZW
#undef FT_CONFIG_OPTION_USE_ZLIB
