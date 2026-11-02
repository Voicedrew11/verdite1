// kf_asset_check DISC_PATH FILE...: runs the check a mod's replacement for DISC_PATH passes
// (mod_assets.cpp, the row the host would pick by suffix) on each FILE. Prints one line a file:
// "ok", or "refused: REASON". scripts/fuzz.py --corrupt-assets filters its mutants through it.
#include "../ext/mod_assets.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

// The row a file takes: the first whose suffix the disc path ends with.
const verdite::ModAssetKind *kind_of(std::string_view disc_path, const std::vector<verdite::ModAssetKind> &kinds) {
    for (const auto &kind : kinds)
        if (disc_path.ends_with(kind.suffix))
            return &kind;
    return nullptr;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: kf_asset_check DISC_PATH FILE...\n");
        return 2;
    }
    const auto kinds = kf::ext::mod_asset_kinds();
    const verdite::ModAssetKind *kind = kind_of(argv[1], kinds);
    if (!kind) {
        std::fprintf(stderr, "%s: no kind of file a mod may replace\n", argv[1]);
        return 2;
    }
    for (int i = 2; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        if (!in) {
            std::printf("refused: cannot be read\n");
            continue;
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto reason = kind->check(bytes);
        std::printf("%s\n", reason ? ("refused: " + *reason).c_str() : "ok");
    }
    return 0;
}
