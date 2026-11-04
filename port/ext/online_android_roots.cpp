// OpenSSL's own certificate folder does not exist on a phone, and the phone's is hashed in a form
// OpenSSL 3 does not look up. So the phone's roots are gathered into one file at each start, which
// keeps them current as the system updates them, and OpenSSL is pointed at it.
#include "internal.hpp"

#include <SDL3/SDL_system.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace kf::ext {
namespace {

// Updatable since Android 14, where the system's folder may be stale; the older place after it.
constexpr std::array<const char *, 2> root_folders = {"/apex/com.android.conscrypt/cacerts",
                                                      "/system/etc/security/cacerts"};

// Each file is a certificate as text and then as PEM; OpenSSL skips to the PEM.
std::size_t append_roots(std::ofstream &out, const char *folder) {
    std::error_code error;
    std::size_t count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(folder, error)) {
        std::ifstream root(entry.path(), std::ios::binary);
        if (!root)
            continue;
        out << root.rdbuf() << '\n';
        ++count;
    }
    return count;
}

} // namespace

void online_trust_system_roots() {
    const char *storage = SDL_GetAndroidInternalStoragePath();
    if (storage == nullptr) {
        std::printf("[online] no internal storage for the root certificates: %s\n", SDL_GetError());
        return;
    }
    const std::string path = std::string(storage) + "/root-certificates.pem";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    std::size_t count = 0;
    for (const char *folder : root_folders) {
        count = append_roots(out, folder);
        if (count != 0)
            break;
    }
    out.close();
    if (!out || count == 0) {
        std::printf("[online] could not gather the root certificates (%zu)\n", count);
        return;
    }
    if (setenv("SSL_CERT_FILE", path.c_str(), 1) != 0)
        std::printf("[online] could not name the root certificates\n");
}

} // namespace kf::ext
