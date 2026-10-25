// libFuzzer: a memory card image from outside the game (an emulator's .mcd, a C# Verdite build's
// carda.sav). Reading one must not crash, and what it reads must write back to a card that reads
// the same.
#include <verdite/memory_card.h>

#include <cstdlib>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const auto read = verdite::read_memory_card(std::span(data, size));
    if (read.files.empty())
        return 0;
    const auto written = verdite::write_memory_card(read.files);
    if (!written)
        return 0;
    const auto again = verdite::read_memory_card(*written);
    if (again.files.size() != read.files.size())
        std::abort();
    for (std::size_t i = 0; i < read.files.size(); ++i)
        if (again.files[i].name != read.files[i].name || again.files[i].bytes != read.files[i].bytes)
            std::abort();
    return 0;
}
