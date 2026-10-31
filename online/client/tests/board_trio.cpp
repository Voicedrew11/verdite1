// vo_board_trio: three boards on one floor of a running service (online/README.md, "Records").
//
//   vo_board_trio --service http://127.0.0.1:8787
//
// Each board opens with its own key in a fresh folder and enters area 7 of game "test". After a
// settle, A posts {1, 2, 3}. B and C must hold it within 10 s: the service does not push posts, so
// they get it by gossip. A's mine list must show it within 10 s; B rates it (again every 2 s while
// the service refuses it, which it does while A's post has not reached it), and A's mine list must
// show one good within 10 s. After A posts ten more, its first post must be gone from its records.
// Exits 0 when all of that holds, else says what failed.

#include "verdite_online.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t trio_area = 7;
constexpr const char *trio_game = "test";
constexpr auto settle_time =
    std::chrono::seconds(5); // the mesh links by then (not observable: see the report)
constexpr auto step_time = std::chrono::seconds(10);
constexpr auto frame_time = std::chrono::milliseconds(20);
constexpr std::size_t listed_most = 64;
constexpr std::array<uint8_t, 3> first_payload = {1, 2, 3};
constexpr uint32_t later_posts = 10;

struct BoardClose {
    void operator()(vo_board *board) const {
        vo_board_close(board);
    }
};
using Board = std::unique_ptr<vo_board, BoardClose>;

// A folder under the system temp dir, removed when the run ends.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::random_device random;
        path = std::filesystem::temp_directory_path() /
               ("verdite-board-trio-" + std::to_string(random()) + std::to_string(random()));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

// The area's records the board holds; reading them pumps the board.
std::vector<vo_record> records_of(vo_board *board) {
    std::vector<vo_record> out(listed_most);
    int changed = 0;
    out.resize(vo_board_records(board, out.data(), out.size(), &changed));
    return out;
}

std::vector<vo_record> mine_of(vo_board *board) {
    std::vector<vo_record> out(listed_most);
    int changed = 0;
    out.resize(vo_board_mine(board, out.data(), out.size(), &changed));
    return out;
}

bool has_record(vo_board *board, vo_record_id id) {
    for (const vo_record &record : records_of(board))
        if (record.id == id)
            return true;
    return false;
}

bool holds_first_post(vo_board *board, vo_record_id id) {
    for (const vo_record &record : records_of(board)) {
        if (record.id == id && record.payload_size == first_payload.size() &&
            std::memcmp(record.payload, first_payload.data(), first_payload.size()) == 0)
            return true;
    }
    return false;
}

// Whether the author's list shows id with at least good ratings.
bool listed_with_good(vo_board *board, vo_record_id id, uint32_t good) {
    for (const vo_record &record : mine_of(board))
        if (record.id == id && record.good >= good)
            return true;
    return false;
}

// Reads every board each frame until done() holds or the time is up.
template <typename Done>
bool wait_for(const std::array<vo_board *, 3> &boards, Clock::duration within, Done done) {
    const auto until = Clock::now() + within;
    for (;;) {
        if (done())
            return true;
        if (Clock::now() >= until)
            return false;
        for (vo_board *board : boards)
            vo_board_records(board, nullptr, 0, nullptr); // pumps the board
        std::this_thread::sleep_for(frame_time);
    }
}

void report(bool ok, const char *what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    std::fflush(stdout);
}

std::string service_from(int argc, char **argv) {
    std::string service;
    for (int i = 1; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--service")
            service = argv[i + 1];
    return service;
}

} // namespace

int main(int argc, char **argv) {
    const std::string service = service_from(argc, argv);
    if (service.empty()) {
        std::fprintf(stderr, "usage: vo_board_trio --service URL\n");
        return 2;
    }
    TempDir dir; // outlives the boards, which close first
    std::array<Board, 3> boards;
    const std::array<const char *, 3> names = {"a", "b", "c"};
    std::array<vo_board *, 3> raw{};
    for (std::size_t i = 0; i < boards.size(); ++i) {
        const std::string key = (dir.path / (std::string("key-") + names[i])).string();
        boards[i].reset(vo_board_open(service.c_str(), trio_game, key.c_str()));
        raw[i] = boards[i].get();
        if (raw[i] == nullptr) {
            std::printf("FAIL: board %s did not open\n", names[i]);
            return 1;
        }
    }
    for (vo_board *board : raw)
        vo_board_enter(board, trio_area);
    for (const auto until = Clock::now() + settle_time; Clock::now() < until;) {
        for (vo_board *board : raw)
            vo_board_records(board, nullptr, 0, nullptr);
        std::this_thread::sleep_for(frame_time);
    }

    const vo_record_id first =
        vo_board_post(raw[0], 42, first_payload.data(), first_payload.size());
    if (first == 0) {
        report(false, "A posted {1, 2, 3}");
        return 1;
    }
    report(true, "A posted {1, 2, 3}");

    bool ok = true;
    const auto check = [&ok](bool passed, const char *what) {
        report(passed, what);
        ok = ok && passed;
    };
    check(wait_for(raw, step_time, [&] { return holds_first_post(raw[1], first); }),
          "B holds the post within 10 s, by gossip");
    check(wait_for(raw, step_time, [&] { return holds_first_post(raw[2], first); }),
          "C holds the post within 10 s, by gossip");
    check(wait_for(raw, step_time, [&] { return listed_with_good(raw[0], first, 0); }),
          "A's mine lists the post within 10 s");

    // A's own list shows the post before the service has it, and gossip can bring it to B sooner
    // still, so a rating can arrive first and be refused: B rates again every 2 s until it counts.
    auto rated_at = Clock::time_point{};
    check(wait_for(raw, step_time,
                   [&] {
                       if (Clock::now() - rated_at >= std::chrono::seconds(2)) {
                           vo_board_rate(raw[1], first);
                           rated_at = Clock::now();
                       }
                       return listed_with_good(raw[0], first, 1);
                   }),
          "A's mine shows the post good 1 within 10 s, after B rated it");

    bool posted_more = true;
    for (uint32_t k = 0; k < later_posts; ++k)
        posted_more =
            vo_board_post(raw[0], 100 + k, first_payload.data(), first_payload.size()) != 0 &&
            posted_more;
    check(posted_more, "A posted ten more");
    check(!has_record(raw[0], first), "A's first post is retired from its records");

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
