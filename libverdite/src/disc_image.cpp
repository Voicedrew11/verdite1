#include <verdite/disc_image.h>

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace verdite {
namespace {

constexpr std::array<char, 8> chd_magic = {'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};
constexpr std::uint32_t raw_sector_bytes = 2352;
constexpr std::uint32_t cooked_sector_bytes = 2048;
constexpr std::uint32_t track_metadata_capacity = 256;
constexpr std::uint32_t no_hunk = std::numeric_limits<std::uint32_t>::max();

struct FileCloser {
    void operator()(FILE *file) const { std::fclose(file); }
};
using File = std::unique_ptr<FILE, FileCloser>;

struct ChdCloser {
    void operator()(chd_file *chd) const { chd_close(chd); }
};
using Chd = std::unique_ptr<chd_file, ChdCloser>;

struct OpenedFile {
    File file;
    std::uint64_t size_bytes = 0;
};

#ifdef _WIN32
std::optional<OpenedFile> open_file(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
        return std::nullopt;
    File file(::_wfopen(path.c_str(), L"rb"));
    if (!file)
        return std::nullopt;
    return OpenedFile{std::move(file), size};
}

bool seek(FILE *file, std::uint64_t offset) {
    return ::_fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
}
#else
// O_NONBLOCK: a FIFO named like an image must not hang the open.
std::optional<OpenedFile> open_file(const std::filesystem::path &path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0)
        return std::nullopt;
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0) {
        ::close(descriptor);
        return std::nullopt;
    }
    File file(::fdopen(descriptor, "rb"));
    if (!file) {
        ::close(descriptor);
        return std::nullopt;
    }
    return OpenedFile{std::move(file), static_cast<std::uint64_t>(info.st_size)};
}

bool seek(FILE *file, std::uint64_t offset) {
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
}
#endif

bool in_bounds(std::uint64_t offset, std::size_t length, std::uint64_t size) {
    return offset <= size && length <= size - offset;
}

class PlainImage final : public DiscImage {
public:
    explicit PlainImage(OpenedFile opened) : file_(std::move(opened.file)), size_(opened.size_bytes) {}

    std::uint64_t size_bytes() const override { return size_; }

    bool read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (!in_bounds(offset, out.size(), size_) || !seek(file_.get(), offset))
            return false;
        return std::fread(out.data(), 1, out.size(), file_.get()) == out.size();
    }

private:
    File file_;
    std::uint64_t size_;
};

// Where the first track's sectors are among the CHD's frames, and how many
// bytes of each frame are the sector.
struct ChdTrack {
    std::uint32_t first_frame = 0;
    std::uint32_t frames = 0;
    std::uint32_t sector_bytes = 0;
};

class ChdImage final : public DiscImage {
public:
    ChdImage(File file, Chd chd, ChdTrack track, std::uint32_t hunk_bytes)
        : file_(std::move(file)), chd_(std::move(chd)), track_(track),
          frames_per_hunk_(hunk_bytes / CD_FRAME_SIZE), hunk_(hunk_bytes) {}

    std::uint64_t size_bytes() const override {
        return std::uint64_t{track_.frames} * track_.sector_bytes;
    }

    bool read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        if (!in_bounds(offset, out.size(), size_bytes()))
            return false;
        while (!out.empty()) {
            const auto sector = static_cast<std::uint32_t>(offset / track_.sector_bytes);
            const auto within = static_cast<std::uint32_t>(offset % track_.sector_bytes);
            const std::uint32_t frame = track_.first_frame + sector;
            if (!load_hunk(frame / frames_per_hunk_))
                return false;
            const std::size_t count = std::min<std::size_t>(track_.sector_bytes - within, out.size());
            const std::size_t at = std::size_t{frame % frames_per_hunk_} * CD_FRAME_SIZE + within;
            std::memcpy(out.data(), hunk_.data() + at, count);
            out = out.subspan(count);
            offset += count;
        }
        return true;
    }

private:
    // One hunk cached: files are read whole and in order, so a hunk is decoded once.
    bool load_hunk(std::uint32_t index) {
        if (index == cached_hunk_)
            return true;
        cached_hunk_ = no_hunk;
        if (chd_read(chd_.get(), index, hunk_.data()) != CHDERR_NONE)
            return false;
        cached_hunk_ = index;
        return true;
    }

    File file_;   // declared before chd_ so it outlives it: libchdr reads through it
    Chd chd_;
    ChdTrack track_;
    std::uint32_t frames_per_hunk_;
    std::vector<std::uint8_t> hunk_;
    std::uint32_t cached_hunk_ = no_hunk;
};

std::string_view metadata_field(std::string_view text, std::string_view key) {
    for (std::size_t at = text.find(key); at != std::string_view::npos; at = text.find(key, at + 1)) {
        if (at != 0 && text[at - 1] != ' ')
            continue;
        const auto start = at + key.size();
        return text.substr(start, text.find(' ', start) - start);
    }
    return {};
}

std::optional<std::uint32_t> metadata_number(std::string_view text, std::string_view key) {
    const auto field = metadata_field(text, key);
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (field.empty() || error != std::errc{} || end != field.data() + field.size())
        return std::nullopt;
    return value;
}

std::optional<std::string> first_track_metadata(chd_file *chd) {
    std::array<char, track_metadata_capacity> text{};
    for (const auto tag : {CDROM_TRACK_METADATA2_TAG, CDROM_TRACK_METADATA_TAG}) {
        std::uint32_t length = 0;
        if (chd_get_metadata(chd, tag, 0, text.data(), text.size() - 1, &length, nullptr, nullptr) ==
            CHDERR_NONE) {
            const std::string_view stored(text.data(), std::min<std::size_t>(length, text.size() - 1));
            return std::string(stored.substr(0, stored.find('\0')));
        }
    }
    return std::nullopt;
}

struct TrackType {
    std::string_view name;
    std::uint32_t sector_bytes;
};
constexpr std::array<TrackType, 4> data_track_types = {{
    {"MODE1_RAW", raw_sector_bytes},
    {"MODE2_RAW", raw_sector_bytes},
    {"MODE1", cooked_sector_bytes},
    {"MODE2_FORM1", cooked_sector_bytes},
}};

struct TrackRead {
    std::optional<ChdTrack> track;
    std::string error;
};

TrackRead read_first_track(chd_file *chd, std::uint64_t total_frames) {
    const auto metadata = first_track_metadata(chd);
    if (!metadata)
        return {std::nullopt, "This CHD is not of a CD (make it with chdman createcd)."};
    const auto number = metadata_number(*metadata, "TRACK:");
    const auto frames = metadata_number(*metadata, "FRAMES:");
    const auto type = metadata_field(*metadata, "TYPE:");
    if (number != 1u || !frames || *frames == 0 || type.empty())
        return {std::nullopt, "This CHD's track list is damaged."};
    if (type == "AUDIO")
        return {std::nullopt, "This CHD's first track is audio, not the game's data."};
    std::uint32_t sector_bytes = 0;
    for (const auto &known : data_track_types)
        if (known.name == type)
            sector_bytes = known.sector_bytes;
    if (!sector_bytes)
        return {std::nullopt, "This CHD's first track is " + std::string(type) +
                                  ", which is not a data track that can be read."};
    // A pregap of type V... is stored in the CHD ahead of the track's own frames.
    const auto pregap = metadata_number(*metadata, "PREGAP:").value_or(0);
    const auto first_frame = metadata_field(*metadata, "PGTYPE:").starts_with('V') ? pregap : 0;
    if (std::uint64_t{first_frame} + *frames > total_frames)
        return {std::nullopt, "This CHD's track is longer than the CHD."};
    return {ChdTrack{first_frame, *frames, sector_bytes}, {}};
}

std::string chd_open_error(chd_error error) {
    if (error == CHDERR_REQUIRES_PARENT)
        return "This CHD holds only the differences from another CHD and cannot be read on its own. "
               "Make a complete one with chdman createcd.";
    return std::string("Cannot read this CHD: ") + chd_error_string(error) + ".";
}

DiscImageOpened open_chd(File file, std::uint64_t limit_bytes) {
    chd_file *opened = nullptr;
    if (const auto error = chd_open_file(file.get(), CHD_OPEN_READ, nullptr, &opened); error != CHDERR_NONE)
        return {nullptr, chd_open_error(error)};
    Chd chd(opened);
    const chd_header *header = chd_get_header(chd.get());
    if (!header || header->hunkbytes == 0 || header->hunkbytes % CD_FRAME_SIZE != 0)
        return {nullptr, "This CHD is not of a CD (make it with chdman createcd)."};
    const std::uint64_t total_frames = std::uint64_t{header->totalhunks} * (header->hunkbytes / CD_FRAME_SIZE);
    auto read = read_first_track(chd.get(), total_frames);
    if (!read.track)
        return {nullptr, std::move(read.error)};
    if (std::uint64_t{read.track->frames} * read.track->sector_bytes > limit_bytes)
        return {nullptr, "This CHD is far larger than the game's disc."};
    const auto hunk_bytes = header->hunkbytes;
    return {std::make_unique<ChdImage>(std::move(file), std::move(chd), *read.track, hunk_bytes), {}};
}

bool starts_with_chd_magic(FILE *file) {
    std::array<char, chd_magic.size()> start{};
    return std::fread(start.data(), 1, start.size(), file) == start.size() && start == chd_magic;
}

std::string display_name(const std::filesystem::path &path) {
    const auto name = path.filename().u8string();
    return {name.begin(), name.end()};
}

} // namespace

DiscImageOpened open_disc_image(const std::filesystem::path &path, std::uint64_t limit_bytes) {
    auto opened = open_file(path);
    if (!opened)
        return {nullptr, "Cannot open " + display_name(path) + "."};
    if (opened->size_bytes == 0)
        return {nullptr, display_name(path) + " is empty."};
    if (starts_with_chd_magic(opened->file.get()))
        return open_chd(std::move(opened->file), limit_bytes);
    if (opened->size_bytes > limit_bytes)
        return {nullptr, display_name(path) + " is far larger than the game's disc."};
    return {std::make_unique<PlainImage>(std::move(*opened)), {}};
}

} // namespace verdite
