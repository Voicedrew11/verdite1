#include <kf/platform/patch_archive.h>
#include <algorithm>
#include <atomic>
#include <cctype>
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
std::optional<std::vector<u8>> read_file(const std::filesystem::path &path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > patch_file_limit)
        return std::nullopt;
    std::ifstream stream(path, std::ios::binary);
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    if (!stream ||
        !stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
        return std::nullopt;
    return bytes;
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
               ("verdite1-" + std::to_string(id) + "-" + std::to_string(serial++));
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path, error);
    }
    ~ScratchDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// Runs a tool that ships with the system (curl is part of Windows 10 and later)
// without a console window. Every argument is quoted.
bool run_tool(const char *tool, const std::vector<std::filesystem::path> &arguments) {
#ifdef _WIN32
    wchar_t system[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return false;
    std::wstring command =
        L"\"" + std::wstring(system) + L"\\" + std::filesystem::path(tool).wstring() + L".exe\"";
    for (const auto &argument : arguments)
        command += L" \"" + argument.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process))
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
} // namespace

std::optional<std::vector<u8>> patch_archive_download(const char *url, std::string *error) {
    ScratchDirectory scratch;
    const auto archive = scratch.path / "patch-archive";
    if (!run_tool("curl",
                  {"-fsSL", "--retry", "2", "--connect-timeout", "20", "-o", archive, url})) {
        *error = "The download failed. Check your internet connection, or choose the patch file "
                 "yourself.";
        return std::nullopt;
    }
    auto bytes = read_file(archive);
    if (!bytes)
        *error = "The download failed.";
    return bytes;
}

#ifndef _WIN32 // Windows unpacks in-process (patch_unpack_libarchive.cpp)
std::optional<std::vector<u8>> patch_from_archive(std::span<const u8> archive_bytes,
                                                  std::string *error) {
    ScratchDirectory scratch;
    const auto archive = scratch.path / "patch-archive";
    const auto extracted = scratch.path / "extracted";
    std::error_code failure;
    std::filesystem::create_directories(extracted, failure);
    {
        std::ofstream out(archive, std::ios::binary);
        out.write(reinterpret_cast<const char *>(archive_bytes.data()),
                  static_cast<std::streamsize>(archive_bytes.size()));
        if (!out.flush()) {
            *error = "Cannot read the patch in that archive.";
            return std::nullopt;
        }
    }
    // GNU tar cannot read .rar; the AppImage carries bsdtar.
    if (!run_tool("bsdtar", {"-xf", archive, "-C", extracted})) {
        *error =
            "Cannot open that archive. Extract the .ppf patch from it and choose that instead.";
        return std::nullopt;
    }
    std::filesystem::path best;
    std::uintmax_t best_size = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(extracted, failure)) {
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
        return std::nullopt;
    }
    auto ppf = read_file(best);
    if (!ppf)
        *error = "Cannot read the patch in that archive.";
    return ppf;
}
#endif
} // namespace kf
