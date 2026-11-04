// The AppImage links in OpenSSL from its build image, which looks for root certificates only where
// Debian keeps them, so on other distributions no wss:// connection is trusted. Each distribution
// keeps one file of them; OpenSSL is pointed at the first that exists.
#include "internal.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace kf::ext {
namespace {

constexpr std::array<const char *, 5> root_files = {
    "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, SteamOS, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora, RHEL, without the link above
    "/etc/ssl/ca-bundle.pem",                            // openSUSE
    "/etc/ssl/cert.pem",                                 // Alpine, Void
};

bool is_file(const char *path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

} // namespace

void online_trust_system_roots() {
    if (std::getenv("SSL_CERT_FILE") != nullptr || std::getenv("SSL_CERT_DIR") != nullptr)
        return; // the player's or the nix shell's own choice
    for (const char *path : root_files) {
        if (!is_file(path))
            continue;
        if (setenv("SSL_CERT_FILE", path, 1) != 0)
            std::printf("[online] could not name the root certificates\n");
        return;
    }
    std::printf("[online] no root certificates found; the service may not be reachable\n");
}

} // namespace kf::ext
