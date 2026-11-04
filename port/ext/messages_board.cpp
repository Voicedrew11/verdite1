// Messages on the ground on this machine (docs/MESSAGES_PLAN.md): the board of the floor the player
// is on, the marks the game draws, the action button and the box of the message open for reading,
// the line saying the player's own were rated, and the
// Message Stone's menu (extra_items.cpp's row), which leaves a message on the player's cell. Everything
// here runs on the game thread, as the board does.
#include "internal.hpp"
#include "item_menu.h"
#include "message_reading.h"
#include "message_writer.h"
#include "messages.h"

#include <kf/platform/automap.h>
#include <kf/platform/coop.h>
#include <kf/platform/messages.h>
#include <kf/platform/title.h>
#include <verdite/message_sample.h>
#include <verdite/pickup_feed.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#if KF_ONLINE
#include <verdite_online.h>
#endif

namespace kf::ext {
namespace {

#if KF_ONLINE

constexpr const char *game_name = "kf1";
constexpr std::size_t floor_records_most = 512;   // the most the board is copied to the game each change
constexpr std::size_t mine_most = VO_RECORDS_PER_AUTHOR;

struct BoardClose {
    void operator()(vo_board *board) const { vo_board_close(board); }
};

// A record of the entered floor, decoded: its place and what it says.
struct Held {
    std::uint64_t id = 0;
    MessagePlace place;
    Message message;
    std::uint32_t good = 0;
    bool mine = false;
    bool rated = false;
};

struct Board {
    std::unique_ptr<vo_board, BoardClose> handle;
    bool open_failed = false;
    std::optional<int> floor;                    // the floor entered
    verdite::MessageSample sample{0};           // seeded when the board opens
    std::vector<Held> held;                      // the floor's records that decode and are on the map
    std::vector<Held> shown;                     // those the sample shows, in its order
    std::vector<MessageOut> mine;                // the player's own, out on any floor
    std::vector<MessageCount> mine_counts;       // mine's good counts, as last heard
    MessageBox box;
    std::optional<MessageCell> reached;          // the cell of the messages the action button would open
    PlayerFront front = PlayerFront::Clear;      // as the game last said
    std::set<std::uint64_t> read;                // opened this session
    verdite::PickupFeed notices{verdite::PickupFeedShape{.most_lines = 2, .shown_ms = 4000}};
    bool records_stale = false;                  // the floor was just entered: copy its records
};
Board board;

struct Text {
    std::string_view english, japanese;
};
constexpr Text prompt_text{"READ MESSAGE", "ことばを よむ"};
constexpr Text leave_text{"LEAVE", "とじる"};
constexpr Text good_text{"GOOD", "よい"};
constexpr Text next_text{"NEXT", "つぎ"};
constexpr Text yours_text{"YOUR MESSAGE", "じぶんの ことば"};
constexpr Text found_good_text{"FOUND IT GOOD", "にんが よいと した"};
constexpr Text you_found_good_text{"YOU FOUND IT GOOD", "よいと つたえた"};
constexpr Text you_and_text{"YOU AND ", "あなたと "};
constexpr Text notice_text{"YOUR MESSAGE RATED GOOD", "のこした ことばに よい"};

std::string in_language(const Text &text, Language language) {
    return std::string(language == Language::English ? text.english : text.japanese);
}

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

const Held *find_held(std::span<const Held> records, std::uint64_t id) {
    const auto found = std::ranges::find_if(records, [id](const Held &record) { return record.id == id; });
    return found == records.end() ? nullptr : &*found;
}

MessageRating rating_of(const Held &record) {
    const bool can_rate = !record.mine && !record.rated;
    return can_rate ? MessageRating::Possible : MessageRating::Impossible;
}

std::optional<Held> held_of(const vo_record &record) {
    const auto place = message_place_decode(record.place);
    if (!place)
        return std::nullopt;
    const std::size_t size = std::min<std::size_t>(record.payload_size, VO_RECORD_PAYLOAD_BYTES);
    const auto message = message_decode(std::span<const std::uint8_t>(record.payload, size));
    if (!message)
        return std::nullopt;
    return Held{record.id, *place, *message, record.good, record.mine != 0, record.rated != 0};
}

std::vector<Held> held_of_all(std::span<const vo_record> records) {
    std::vector<Held> held;
    for (const vo_record &record : records)
        if (const auto decoded = held_of(record))
            held.push_back(*decoded);
    return held;
}

// The sample of what the held records show, as ids; the shown records keep the sample's order.
void show_sample() {
    std::vector<verdite::OfferedMessage> offered;
    offered.reserve(board.held.size());
    for (const Held &record : board.held)
        offered.push_back({record.id, record.place.cell.x, record.place.cell.z, record.good, record.mine});
    board.shown.clear();
    for (const std::uint64_t id : board.sample.shown(offered))
        if (const Held *record = find_held(board.held, id))
            board.shown.push_back(*record);
}

// Copies the floor's records when the board changed them, or the floor was just entered.
void refresh_records() {
    std::vector<vo_record> records(floor_records_most);
    int changed = 0;
    const std::size_t count = vo_board_records(board.handle.get(), records.data(), records.size(), &changed);
    if (changed == 0 && !board.records_stale)
        return;
    board.records_stale = false;
    board.held = held_of_all(std::span<const vo_record>(records).first(std::min(count, records.size())));
    show_sample();
}

void refresh_mine() {
    std::vector<vo_record> records(mine_most);
    int changed = 0;
    const std::size_t count = vo_board_mine(board.handle.get(), records.data(), records.size(), &changed);
    if (changed == 0)
        return;
    board.mine.clear();
    std::vector<MessageCount> counts;
    for (const vo_record &record : std::span<const vo_record>(records).first(std::min(count, records.size()))) {
        counts.push_back({record.id, record.good});
        const std::size_t size = std::min<std::size_t>(record.payload_size, VO_RECORD_PAYLOAD_BYTES);
        if (const auto message = message_decode(std::span<const std::uint8_t>(record.payload, size)))
            board.mine.push_back({*message, record.good});
    }
    if (const std::uint32_t gained = message_good_gained(board.mine_counts, counts))
        board.notices.add({.count = gained}, now_ms());
    board.mine_counts = std::move(counts);
}

// The board is opened when a floor is first entered: its key is made then, not at the title.
std::uint64_t session_seed() {
    std::random_device random;
    return (static_cast<std::uint64_t>(random()) << 32) | random();
}

bool open_board() {
    if (board.handle)
        return true;
    if (board.open_failed)
        return false;
    const std::string service = online_service_url();
    const std::string key = (user_dir() / "message_key").string();
    board.handle.reset(vo_board_open(service.c_str(), game_name, key.c_str()));
    if (!board.handle) {
        board.open_failed = true;
        std::printf("[messages] the board cannot be opened (key %s)\n", key.c_str());
        return false;
    }
    board.sample = verdite::MessageSample(session_seed());
    return true;
}

void leave_floor() {
    if (!board.floor)
        return;
    vo_board_leave(board.handle.get());
    board.floor.reset();
    board.held.clear();
    board.shown.clear();
    board.box.close();
    board.reached.reset();
    board.sample.clear();
}

void enter_floor(int floor) {
    leave_floor();
    vo_board_enter(board.handle.get(), static_cast<std::uint32_t>(floor));
    board.floor = floor;
    board.records_stale = true;
}

// The floor the player is on: none at the title or off the map, or with messages off. A coop room
// has them on for everyone, whatever each player's setting says.
std::optional<int> floor_now() {
    if (title_is_open() || !(settings().online_messages || coop_world_in_play()))
        return std::nullopt;
    const int floor = automap_snapshot().floor;
    if (floor < 1 || floor > automap_floor_count)
        return std::nullopt;
    return floor;
}

GroundMarkKind mark_kind_of(const Held &record) {
    if (record.mine)
        return GroundMarkKind::Mine;
    return record.rated || board.read.contains(record.id) ? GroundMarkKind::Read : GroundMarkKind::Unread;
}

std::vector<GroundMark> marks_on(int floor) {
    std::vector<GroundMark> marks;
    if (board.floor != floor)
        return marks;
    for (const Held &record : board.shown)
        marks.push_back({static_cast<u8>(record.place.cell.x), static_cast<u8>(record.place.cell.z),
            record.place.facing, mark_kind_of(record)});
    return marks;
}

MessageRating shown_rating(std::uint64_t id) {
    const Held *record = find_held(board.shown, id);
    return record ? rating_of(*record) : MessageRating::Impossible;
}

bool reach_message(int floor, float x, float z, float yaw, PlayerFront front) {
    board.reached.reset();
    board.front = front;
    if (board.floor != floor || front == PlayerFront::WorldTarget || board.box.is_open())
        return false;
    std::vector<MessageCell> cells;
    cells.reserve(board.shown.size());
    for (const Held &record : board.shown)
        cells.push_back(record.place.cell);
    const MessageReach reach = front == PlayerFront::Wall ? MessageReach::HereOnly : MessageReach::HereAndAhead;
    board.reached = message_cell_for_action(cells, x, z, yaw, reach);
    return board.reached.has_value();
}

void open_reached_cell() {
    if (!board.reached)
        return;
    std::vector<std::uint64_t> ids;
    for (const Held &record : board.shown)
        if (record.place.cell == *board.reached)
            ids.push_back(record.id);
    board.box.open(ids, *board.reached);
    board.reached.reset();
}

void press_box(MessageKey key) {
    const auto shown_now = board.box.current();
    if (!shown_now)
        return;
    if (const auto rate = board.box.press(key, shown_rating(*shown_now)))
        vo_board_rate(board.handle.get(), *rate);
}

// Who found it good: nothing while nobody has, and the player first once they have.
std::string found_good(const Held &record, Language language) {
    if (record.rated && record.good <= 1)
        return in_language(you_found_good_text, language);
    if (record.good == 0)
        return {};
    const std::string others = std::to_string(record.rated ? record.good - 1 : record.good);
    return (record.rated ? in_language(you_and_text, language) : std::string()) + others + " " + in_language(found_good_text, language);
}

// The small line under the message: which of the cell's, whose, and who found it good.
std::string status_of(const Held &record, Language language) {
    std::vector<std::string> parts;
    if (board.box.page_count() > 1) {
        const std::string page = std::to_string(board.box.page() + 1), pages = std::to_string(board.box.page_count());
        parts.push_back(language == Language::English ? page + " OF " + pages : pages + " のうち " + page);
    }
    if (record.mine)
        parts.push_back(in_language(yours_text, language));
    if (std::string found = found_good(record, language); !found.empty())
        parts.push_back(std::move(found));
    std::string status;
    for (const std::string &part : parts)
        status += (status.empty() ? "" : "  ") + part;
    return status;
}

std::string choice_text(MessageChoice choice, Language language) {
    switch (choice) {
    case MessageChoice::Leave:
        return in_language(leave_text, language);
    case MessageChoice::Good:
        return in_language(good_text, language);
    case MessageChoice::Next:
        return in_language(next_text, language);
    }
    return {};
}

std::optional<kf::MessageReading> reading_now(Language language, int floor, float x, float z) {
    if (board.floor != floor)
        return std::nullopt;
    board.box.follow(x, z);
    const auto id = board.box.current();
    if (!id)
        return std::nullopt;
    const Held *record = find_held(board.shown, *id);
    if (!record) {
        board.box.close();
        return std::nullopt;
    }
    board.read.insert(*id);
    kf::MessageReading reading;
    for (const MessagePiece &piece : message_pieces(record->message, language))
        reading.pieces.push_back({piece.text, piece.item});
    reading.status = status_of(*record, language);
    const MessageRating rating = rating_of(*record);
    for (const MessageChoice choice : board.box.choices(rating))
        reading.choices.push_back(choice_text(choice, language));
    reading.cursor = board.box.cursor(rating);
    return reading;
}

// Posts a message on the player's cell, facing where they look. False, and a line on the console,
// when it cannot be left here.
bool post_here(const Message &message) {
    const AutomapSnapshot &map = automap_snapshot();
    const auto place = message_place_encode(message_cell_at(map.player_x, map.player_z), map.player_yaw);
    if (!place) {
        std::printf("[messages] not left: the player is off the map\n");
        return false;
    }
    const auto payload = message_encode(message);
    if (vo_board_post(board.handle.get(), *place, payload.data(), payload.size()) == 0) {
        std::printf("[messages] not left: no floor is entered\n");
        return false;
    }
    return true;
}

// The Message Stone's menu: the writer's pages, and a message it finishes is left here.
class StoneMenu final : public ItemMenu {
public:
    StoneMenu() : writer_(item_menu_visible_rows) {}

    ItemMenuState press(ItemMenuKey key) override {
        const MessageWriter::Pressed pressed = writer_.press(key, board.mine);
        if (pressed.state != ItemMenuState::Done || !pressed.left)
            return pressed.state;
        return post_here(*pressed.left) ? ItemMenuState::Done : ItemMenuState::Closed;
    }

    ItemMenuPage page(Language language) const override {
        return writer_.page(language, board.mine);
    }

private:
    MessageWriter writer_;
};

#endif

} // namespace

#if KF_ONLINE

void messages_poll() {
    const std::optional<int> floor = floor_now();
    if (!floor) {
        leave_floor();
        return;
    }
    if (!open_board())
        return;
    if (board.floor != floor)
        enter_floor(*floor);
    refresh_records();
    refresh_mine();
    const AutomapSnapshot &map = automap_snapshot();
    board.box.follow(map.player_x, map.player_z);   // a message closes once the player is moved off
}

bool messages_stone_offered() {
    return board.floor.has_value();
}

std::unique_ptr<ItemMenu> messages_stone_open() {
    if (!board.floor)
        return nullptr;
    return std::make_unique<StoneMenu>();
}

bool messages_leave_here(std::uint8_t template_id, std::uint16_t first_word, std::uint16_t second_word) {
    const auto checked = message_decode(message_encode(Message{template_id, {first_word, second_word}}));
    return board.floor && checked && post_here(*checked);
}

std::string messages_json() {
    std::string marks;
    for (const Held &record : board.shown) {
        const char *kind = record.mine ? "mine" : (mark_kind_of(record) == GroundMarkKind::Read ? "read" : "unread");
        marks += (marks.empty() ? "" : ",") + std::string("{\"x\":") + std::to_string(record.place.cell.x) +
                 ",\"z\":" + std::to_string(record.place.cell.z) + ",\"kind\":\"" + kind +
                 "\",\"good\":" + std::to_string(record.good) + ",\"rated\":" + (record.rated ? "true" : "false") + "}";
    }
    std::string mine;
    for (const MessageOut &out : board.mine)
        mine += (mine.empty() ? "" : ",") + std::to_string(out.good);
    std::string box = "null";
    const AutomapSnapshot &map = automap_snapshot();
    if (const auto reading = board.floor ? kf::messages_reading(Language::English, *board.floor, map.player_x, map.player_z) : std::nullopt) {
        std::string choices;
        for (const std::string &choice : reading->choices)
            choices += (choices.empty() ? "\"" : ",\"") + choice + "\"";
        box = "{\"status\":\"" + reading->status + "\",\"choices\":[" + choices + "],\"cursor\":" + std::to_string(reading->cursor) + "}";
    }
    std::string notices;
    for (const kf::MessageNotice &notice : kf::messages_notices(Language::English))
        notices += (notices.empty() ? "\"" : ",\"") + notice.text + "\"";
    constexpr std::array front_names = {"clear", "wall", "world"};
    return "\"floor\":" + std::to_string(board.floor.value_or(0)) + ",\"front\":\"" + front_names[static_cast<std::size_t>(board.front)] + "\",\"marks\":[" + marks + "],\"mine\":[" + mine +
           "],\"prompt\":\"" + kf::messages_prompt(Language::English) + "\",\"box\":" + box + ",\"notices\":[" + notices + "]";
}

#else

void messages_poll() {}

bool messages_stone_offered() {
    return false;
}

std::unique_ptr<ItemMenu> messages_stone_open() {
    return nullptr;
}

bool messages_leave_here(std::uint8_t, std::uint16_t, std::uint16_t) {
    return false;
}

std::string messages_json() {
    return "\"floor\":0";
}

#endif

} // namespace kf::ext

namespace kf {

#if KF_ONLINE

std::vector<GroundMark> messages_marks(int floor) {
    return ext::marks_on(floor);
}

bool messages_reach(int floor, float x, float z, float yaw, PlayerFront front) {
    return ext::reach_message(floor, x, z, yaw, front);
}

void messages_open_reached() {
    ext::open_reached_cell();
}

bool messages_box_open() {
    return ext::board.box.is_open();
}

void messages_box_press(MessageBoxKey key) {
    constexpr std::array keys = {ext::MessageKey::Previous, ext::MessageKey::Following, ext::MessageKey::Confirm, ext::MessageKey::Back};
    ext::press_box(keys[static_cast<std::size_t>(key)]);
}

void messages_box_close() {
    ext::board.box.close();
    ext::board.reached.reset();
}

std::optional<MessageReading> messages_reading(Language language, int floor, float x, float z) {
    return ext::reading_now(language, floor, x, z);
}

std::string messages_prompt(Language language) {
    return ext::board.reached ? ext::in_language(ext::prompt_text, language) : std::string();
}

std::vector<MessageNotice> messages_notices(Language language) {
    std::vector<MessageNotice> notices;
    for (const verdite::PickupShown &shown : ext::board.notices.shown(ext::now_ms())) {
        const std::uint32_t count = shown.line.count;
        notices.push_back({ext::in_language(ext::notice_text, language) + (count > 1 ? " " + std::to_string(count) : ""), shown.opacity});
    }
    return notices;
}

#else

std::vector<GroundMark> messages_marks(int) {
    return {};
}

bool messages_reach(int, float, float, float, PlayerFront) {
    return false;
}

void messages_open_reached() {}

bool messages_box_open() {
    return false;
}

void messages_box_press(MessageBoxKey) {}

void messages_box_close() {}

std::optional<MessageReading> messages_reading(Language, int, float, float) {
    return std::nullopt;
}

std::string messages_prompt(Language) {
    return {};
}

std::vector<MessageNotice> messages_notices(Language) {
    return {};
}

#endif

} // namespace kf
