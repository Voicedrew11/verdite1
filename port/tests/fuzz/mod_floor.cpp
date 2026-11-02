// libFuzzer: what a mod's folder and a save's side file hold. The check must refuse or accept any bytes
// without a crash: it runs before a stranger's floor is in use. Input: 4 cut points, then the bytes.
#include "../../ext/mod_content_save.h"
#include "../../ext/mod_floor_check.h"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    if (const auto save = kf::ext::content_save_decode(bytes)) {
        const auto again = kf::ext::content_save_decode(kf::ext::content_save_encode(*save));
        if (!again || *again != *save)
            __builtin_trap();
    }
    if (size < 3)
        return 0;
    kf::ext::ModFloorNeeds needs;
    needs.tim_bytes = needs.mixa_bytes = needs.mixb_bytes = needs.seq_bytes = size;
    const std::size_t a = size * data[0] / 256, b = a + (size - a) * data[1] / 256;
    (void)kf::ext::mod_floor_refusal({bytes.first(a), bytes.subspan(a, b - a), bytes.subspan(b), bytes.first(a)}, needs, data[2], data[2]);
    (void)kf::ext::mod_floor_needs_add(needs, {bytes.first(a), bytes.subspan(a, b - a), bytes.subspan(b), bytes.first(a)});
    (void)kf::ext::mod_floor_needs_read_objects(needs, bytes);
    return 0;
}
