#include <kf/platform/picked_file.h>
#include <kf/platform/user_folder.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace kf {
namespace {
std::filesystem::path copy_path() {
    return user_folder() / "picked-file";
}

// Until the stream's end; false on a read or write failure.
bool copy_stream(SDL_IOStream *in, std::ofstream &out) {
    std::vector<char> chunk(std::size_t{1} << 20);
    while (out) {
        const std::size_t got = SDL_ReadIO(in, chunk.data(), chunk.size());
        if (got == 0)
            return SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF;
        out.write(chunk.data(), static_cast<std::streamsize>(got));
    }
    return false;
}
} // namespace

std::string picked_file_path(const std::string &picked) {
    if (picked.rfind("content://", 0) != 0)
        return picked;
    SDL_IOStream *in = SDL_IOFromFile(picked.c_str(), "rb");
    if (!in) {
        std::printf("Cannot open the chosen file: %s\n", SDL_GetError());
        return {};
    }
    const auto copy = copy_path();
    std::ofstream out(copy, std::ios::binary | std::ios::trunc);
    bool ok = copy_stream(in, out);
    ok = SDL_CloseIO(in) && ok;
    out.close();
    const auto text = copy.u8string();
    if (!ok || out.fail()) {
        std::printf("Cannot copy the chosen file into %s\n",
                    reinterpret_cast<const char *>(text.c_str()));
        picked_file_remove_copy();
        return {};
    }
    return {text.begin(), text.end()};
}

void picked_file_remove_copy() {
    std::error_code error;
    std::filesystem::remove(copy_path(), error);
}
} // namespace kf
