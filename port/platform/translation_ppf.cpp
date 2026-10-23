#include <kf/platform/translation_ppf.h>
#include <kf/platform/disc_layout.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace kf {
namespace {
constexpr std::size_t raw_sector_bytes = 2352;
constexpr std::size_t sector_data_offset = 24;   // MODE2 form 1: sync, header, subheader
constexpr std::size_t sector_data_bytes = 2048;
constexpr std::size_t ppf_header_bytes = 0x3C;
constexpr std::size_t ppf_block_check_bytes = 1024;
constexpr std::size_t patch_file_limit = 64 * 1024 * 1024;

std::filesystem::path utf8_path(const char *path) {
    return std::filesystem::path(reinterpret_cast<const char8_t *>(path));
}

bool read_file(const std::filesystem::path &path, std::vector<u8> *bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > patch_file_limit)
        return false;
    std::ifstream stream(path, std::ios::binary);
    bytes->resize(static_cast<std::size_t>(size));
    return stream && stream.read(reinterpret_cast<char *>(bytes->data()), static_cast<std::streamsize>(size));
}

// A private scratch folder, removed when this goes out of scope.
struct ScratchDirectory {
    std::filesystem::path path;
    ScratchDirectory() {
        std::error_code error;
#ifdef _WIN32
        const auto id = static_cast<unsigned long>(GetCurrentProcessId());
#else
        const auto id = static_cast<unsigned long>(::getpid());
#endif
        static std::atomic<unsigned> serial{0};
        path = std::filesystem::temp_directory_path(error) /
            ("kings-field-enhanced-" + std::to_string(id) + "-" + std::to_string(serial++));
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// Runs a tool that ships with the system (curl and bsdtar are part of Windows 10
// and later) without a console window. Every argument is quoted.
bool run_tool(const char *tool, const std::vector<std::filesystem::path> &arguments) {
#ifdef _WIN32
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;
    std::wstring command = L"\"" + std::wstring(system) + L"\\" + std::filesystem::path(tool).wstring() + L".exe\"";
    for (const auto &argument : arguments)
        command += L" \"" + argument.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code == 0;
#else
    std::string command = tool;
    for (const auto &argument : arguments)
        command += " '" + argument.string() + "'";
    return std::system(command.c_str()) == 0;
#endif
}

// Extracts an archive and reads the largest .ppf inside it.
bool patch_from_archive(const std::filesystem::path &archive, std::vector<u8> *ppf, std::string *error) {
    ScratchDirectory scratch;
    if (!run_tool("tar", {"-xf", archive, "-C", scratch.path})) {
        *error = "Cannot open that archive. Extract the .ppf patch from it and choose that instead.";
        return false;
    }
    std::error_code failure;
    std::filesystem::path best;
    std::uintmax_t best_size = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(scratch.path, failure)) {
        auto extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::error_code size_error;
        const auto size = entry.is_regular_file(size_error) ? entry.file_size(size_error) : 0;
        if (extension == ".ppf" && !size_error && size > best_size) {
            best = entry.path();
            best_size = size;
        }
    }
    if (best.empty()) {
        *error = "That archive holds no .ppf patch.";
        return false;
    }
    if (!read_file(best, ppf)) {
        *error = "Cannot read the patch in that archive.";
        return false;
    }
    return true;
}
}

bool translation_apply_ppf(AssetTable &assets, const std::vector<u8> &ppf, std::string *error) {
    if (assets_classify(assets) != DiscMatch::Japanese) {
        *error = "The English patch applies to the untouched Japanese game files.";
        return false;
    }
    if (ppf.size() < ppf_header_bytes || std::memcmp(ppf.data(), "PPF30", 5) != 0 || ppf[5] != 2) {
        *error = "That file is not a PPF 3.0 patch.";
        return false;
    }
    // Which file (and which of its sectors) each disc sector belongs to.
    std::vector<Asset *> files;
    std::vector<s32> owner;
    for (const auto &entry : slps00017_layout) {
        auto *asset = assets_find(assets, entry.path);
        if (!asset || asset->bytes.size() != entry.size) {
            *error = "The Japanese game files are incomplete.";
            return false;
        }
        const std::size_t sectors = std::max<std::size_t>(1, (entry.size + sector_data_bytes - 1) / sector_data_bytes);
        if (owner.size() < entry.sector + sectors)
            owner.resize(entry.sector + sectors, -1);
        for (std::size_t i = 0; i < sectors; ++i)
            owner[entry.sector + i] = static_cast<s32>(files.size());
        files.push_back(asset);
    }
    std::size_t position = ppf_header_bytes + (ppf[0x39] ? ppf_block_check_bytes : 0);
    const bool undo = ppf[0x3A] != 0;
    std::size_t end = ppf.size();
    static const char description[] = "@BEGIN_FILE_ID.DIZ";
    if (auto found = std::search(ppf.begin(), ppf.end(), description, description + sizeof description - 1); found != ppf.end())
        end = static_cast<std::size_t>(found - ppf.begin());
    std::size_t changed = 0;
    while (position < end) {
        if (position + 9 > end) {
            *error = "The patch is truncated.";
            return false;
        }
        std::uint64_t offset = 0;
        for (int i = 7; i >= 0; --i)
            offset = offset << 8 | ppf[position + i];
        const std::size_t length = ppf[position + 8];
        position += 9;
        if (position + length > end) {
            *error = "The patch is truncated.";
            return false;
        }
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint64_t at = offset + i;
            const std::uint64_t sector = at / raw_sector_bytes;
            const std::size_t within = static_cast<std::size_t>(at % raw_sector_bytes);
            // Sector headers and error correction are not part of the files.
            if (within < sector_data_offset || within >= sector_data_offset + sector_data_bytes ||
                sector >= owner.size() || owner[sector] < 0)
                continue;
            const auto &entry = slps00017_layout[owner[sector]];
            const std::size_t file_offset = (sector - entry.sector) * sector_data_bytes + within - sector_data_offset;
            if (file_offset < entry.size) {
                files[owner[sector]]->bytes[file_offset] = ppf[position + i];
                ++changed;
            }
        }
        position += length * (undo ? 2 : 1);
    }
    if (!changed || assets_classify(assets) != DiscMatch::English) {
        *error = "The patch did not produce the English translation v1.0. Use John Osborne's "
                 "\"KF Jap to Eng v1.0\" patch.";
        return false;
    }
    return true;
}

bool translation_read_patch(const char *path, std::vector<u8> *ppf, std::string *error) {
    const auto file = utf8_path(path);
    auto extension = file.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".ppf") {
        if (!read_file(file, ppf)) {
            *error = "Cannot read the patch file.";
            return false;
        }
        return true;
    }
    return patch_from_archive(file, ppf, error);
}

bool translation_download(std::vector<u8> *ppf, std::string *error) {
    ScratchDirectory scratch;
    const auto archive = scratch.path / "Kings_Field_Jap_to_Eng_v1.0.rar";
    if (!run_tool("curl", {"-fsSL", "--retry", "2", "--connect-timeout", "20", "-o", archive, english_patch_url})) {
        *error = "The download failed. Check your internet connection, or choose the patch file yourself.";
        return false;
    }
    std::vector<u8> bytes;
    if (!read_file(archive, &bytes)) {
        *error = "The download failed.";
        return false;
    }
    Sha256 hash{};
    sha256_init(&hash);
    sha256_update(&hash, bytes);
    std::array<char, sha256_hex_capacity> digest;
    sha256_finish(&hash, digest);
    if (std::strcmp(digest.data(), english_patch_archive_sha256) != 0) {
        *error = "The downloaded file is not the translator's v1.0 release.";
        return false;
    }
    return patch_from_archive(archive, ppf, error);
}
}
