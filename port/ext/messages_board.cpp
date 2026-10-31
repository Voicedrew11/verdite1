// Messages on the ground on this machine (docs/MESSAGES_PLAN.md): the board of the floor the player
// is on, the marks the game draws, the action button and the message open for reading, and the
// "Leave a message" window. Everything here runs on the game thread, as the board does.
#include "internal.hpp"
#include "message_reading.h"
#include "messages.h"

#include <imgui.h>
#include <kf/platform/automap.h>
#include <kf/platform/messages.h>
#include <kf/platform/title.h>
#include <verdite/message_sample.h>

#include <algorithm>
#include <array>
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

// The writer's kinds of word, in the order its list shows them.
constexpr std::array<MessageWordKind, 7> writer_kinds = {
    MessageWordKind::Direction, MessageWordKind::Feature, MessageWordKind::Action, MessageWordKind::Enemy,
    MessageWordKind::Person, MessageWordKind::Condition, MessageWordKind::Item};
constexpr std::array<const char *, 7> writer_kind_names = {
    "Direction", "Feature", "Action", "Enemy", "Person", "Condition", "Item"};
static_assert(writer_kinds.size() == writer_kind_names.size());

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
};

// One of the player's own records, out on any floor.
struct MineLine {
    Message message;
    std::uint32_t good = 0;
};

// What the writer has chosen so far.
struct Draft {
    std::optional<std::size_t> template_index;
    Message message;                     // its words are message_no_word until chosen
    std::array<std::size_t, 2> kind{};   // each blank's kind: an index into writer_kinds
};

struct Board {
    std::unique_ptr<vo_board, BoardClose> handle;
    bool open_failed = false;
    std::optional<int> floor;                    // the floor entered
    verdite::MessageSample sample{0};           // seeded when the board opens
    std::vector<Held> held;                      // the floor's records that decode and are on the map
    std::vector<Held> shown;                     // those the sample shows, in its order
    std::vector<MineLine> mine;
    OpenMessage reading;
    std::set<std::uint64_t> rated;               // rated by this player this session
    bool records_stale = false;                  // the floor was just entered: copy its records
    Draft draft;
    std::string status;                          // the writer's last line
};
Board board;

// The English line a message reads as, in the writer and the player's list.
std::string english_line(const Message &message) {
    std::string line;
    for (const MessagePiece &piece : message_pieces(message, Language::English))
        line += piece.item == message_no_item ? piece.text : automap_item_name(piece.item);
    return line;
}

std::string english_word(std::uint16_t word) {
    const MessagePiece piece = message_word_piece(word, Language::English);
    return piece.item == message_no_item ? piece.text : automap_item_name(piece.item);
}

// The line under the reading: what the action button does now.
std::string prompt_for(Language language, bool can_rate) {
    if (language == Language::English)
        return can_rate ? "ACTION  GOOD" : "ACTION  CLOSE";
    return can_rate ? "けつていで よい" : "けつていで とじる";
}

const Held *find_held(std::span<const Held> records, std::uint64_t id) {
    const auto found = std::ranges::find_if(records, [id](const Held &record) { return record.id == id; });
    return found == records.end() ? nullptr : &*found;
}

MessageRating rating_of(const Held &record) {
    const bool can_rate = !record.mine && !board.rated.contains(record.id);
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
    return Held{record.id, *place, *message, record.good, record.mine != 0};
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
    for (const vo_record &record : std::span<const vo_record>(records).first(std::min(count, records.size()))) {
        const std::size_t size = std::min<std::size_t>(record.payload_size, VO_RECORD_PAYLOAD_BYTES);
        if (const auto message = message_decode(std::span<const std::uint8_t>(record.payload, size)))
            board.mine.push_back({*message, record.good});
    }
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
    board.reading.close();
    board.draft = Draft{};
    board.sample.clear();
}

void enter_floor(int floor) {
    leave_floor();
    vo_board_enter(board.handle.get(), static_cast<std::uint32_t>(floor));
    board.floor = floor;
    board.records_stale = true;
}

// The floor the player is on: none at the title or off the map.
std::optional<int> floor_now() {
    if (title_is_open() || !settings().online_messages)
        return std::nullopt;
    const int floor = automap_snapshot().floor;
    if (floor < 1 || floor > automap_floor_count)
        return std::nullopt;
    return floor;
}

std::vector<GroundMark> marks_on(int floor) {
    std::vector<GroundMark> marks;
    if (board.floor != floor)
        return marks;
    for (const Held &record : board.shown)
        marks.push_back({static_cast<u8>(record.place.cell.x), static_cast<u8>(record.place.cell.z),
            record.place.facing, record.mine});
    return marks;
}

// The action button with a message open: rates it when it can be rated, and closes it.
void press_open_message(const OpenMessage::Open &open) {
    const Held *record = find_held(board.shown, open.id);
    const MessageRating rating = record ? rating_of(*record) : MessageRating::Impossible;
    if (board.reading.press(rating) == MessagePress::Rate) {
        vo_board_rate(board.handle.get(), open.id);
        board.rated.insert(open.id);
    }
}

// The action button with nothing open: opens the message it is for, if there is one.
bool open_message_for_action(float x, float z, float yaw) {
    std::vector<MessageCell> cells;
    cells.reserve(board.shown.size());
    for (const Held &record : board.shown)
        cells.push_back(record.place.cell);
    const auto index = message_for_action(cells, x, z, yaw);
    if (!index)
        return false;
    const Held &record = board.shown[*index];
    board.reading.open(record.id, record.place.cell);
    return true;
}

std::optional<kf::MessageReading> reading_now(Language language, int floor, float x, float z) {
    if (board.floor != floor)
        return std::nullopt;
    board.reading.follow(x, z);
    const auto open = board.reading.current();
    if (!open)
        return std::nullopt;
    const Held *record = find_held(board.shown, open->id);
    if (!record) {
        board.reading.close();
        return std::nullopt;
    }
    kf::MessageReading reading;
    for (const MessagePiece &piece : message_pieces(record->message, language))
        reading.pieces.push_back({piece.text, piece.item});
    reading.prompt = prompt_for(language, rating_of(*record) == MessageRating::Possible);
    return reading;
}

// --- the writer ---

void choose_template(std::size_t index) {
    board.draft = Draft{};
    board.draft.template_index = index;
    board.draft.message.template_id = static_cast<std::uint8_t>(index);
}

void draw_template_choice() {
    const auto templates = message_templates();
    const std::optional<std::size_t> chosen = board.draft.template_index;
    const std::string preview = chosen ? std::string(templates[*chosen].english) : "(choose)";
    if (!ImGui::BeginCombo("Template", preview.c_str()))
        return;
    for (std::size_t index = 0; index < templates.size(); ++index) {
        const std::string label(templates[index].english);
        if (ImGui::Selectable(label.c_str(), chosen == index))
            choose_template(index);
    }
    ImGui::EndCombo();
}

void draw_kind(std::size_t blank) {
    std::size_t &kind = board.draft.kind[blank];
    if (!ImGui::BeginCombo("Kind", writer_kind_names[kind]))
        return;
    for (std::size_t index = 0; index < writer_kind_names.size(); ++index) {
        if (ImGui::Selectable(writer_kind_names[index], kind == index) && kind != index) {
            kind = index;
            board.draft.message.words[blank] = message_no_word;
        }
    }
    ImGui::EndCombo();
}

void draw_word(std::size_t blank) {
    std::uint16_t &chosen = board.draft.message.words[blank];
    const std::string preview = chosen == message_no_word ? "(choose)" : english_word(chosen);
    if (!ImGui::BeginCombo("Word", preview.c_str()))
        return;
    for (const MessageWord &word : message_words(writer_kinds[board.draft.kind[blank]])) {
        const std::string label = english_word(word.id);
        if (ImGui::Selectable(label.c_str(), chosen == word.id))
            chosen = word.id;
    }
    ImGui::EndCombo();
}

void draw_blank(std::size_t blank) {
    ImGui::PushID(static_cast<int>(blank));
    draw_kind(blank);
    draw_word(blank);
    ImGui::PopID();
}

// Whether the draft has a template and a word for each of its blanks.
bool draft_complete() {
    const Draft &draft = board.draft;
    if (!draft.template_index)
        return false;
    const std::size_t blanks = message_templates()[*draft.template_index].blanks;
    return std::ranges::none_of(std::span(draft.message.words).first(blanks),
        [](std::uint16_t word) { return word == message_no_word; });
}

// Posts the draft on the player's cell, facing where they look.
void leave_here() {
    const AutomapSnapshot &map = automap_snapshot();
    const auto place = message_place_encode(message_cell_at(map.player_x, map.player_z), map.player_yaw);
    if (!place) {
        board.status = "Nothing was left: you are off the map.";
        return;
    }
    const auto payload = message_encode(board.draft.message);
    if (vo_board_post(board.handle.get(), *place, payload.data(), payload.size()) == 0) {
        board.status = "Nothing was left: no floor is entered.";
        return;
    }
    board.status = "Left here.";
    board.draft = Draft{};
}

void draw_writer(int floor) {
    ImGui::TextDisabled("Floor %d. Left on the cell you stand on, facing where you look.", floor);
    draw_template_choice();
    if (board.draft.template_index) {
        const std::size_t blanks = message_templates()[*board.draft.template_index].blanks;
        for (std::size_t blank = 0; blank < blanks; ++blank)
            draw_blank(blank);
    }
    if (draft_complete())
        ImGui::TextWrapped("Preview: %s", english_line(board.draft.message).c_str());
    ImGui::BeginDisabled(!draft_complete());
    if (ImGui::Button("Leave it here"))
        leave_here();
    ImGui::EndDisabled();
    if (!board.status.empty())
        ImGui::TextWrapped("%s", board.status.c_str());
}

void draw_mine() {
    ImGui::Separator();
    ImGui::TextUnformatted("Your messages");
    if (board.mine.empty()) {
        ImGui::TextDisabled("None out.");
        return;
    }
    for (const MineLine &line : board.mine)
        ImGui::BulletText("%s  good %u", english_line(line.message).c_str(), line.good);
}

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
    board.reading.follow(map.player_x, map.player_z);   // a message closes once the player walks off
}

bool messages_available() {
    return board.floor.has_value();
}

void messages_draw_panel() {
    if (!board.floor)
        return;
    draw_writer(*board.floor);
    draw_mine();
}

#else

void messages_poll() {}

bool messages_available() {
    return false;
}

void messages_draw_panel() {
    ImGui::TextWrapped("This build has no online play: messages on the ground need it.");
}

#endif

} // namespace kf::ext

namespace kf {

#if KF_ONLINE

std::vector<GroundMark> messages_marks(int floor) {
    return ext::marks_on(floor);
}

bool messages_action(int floor, float x, float z, float yaw) {
    if (ext::board.floor != floor)
        return false;
    if (const auto open = ext::board.reading.current()) {
        ext::press_open_message(*open);
        return true;
    }
    return ext::open_message_for_action(x, z, yaw);
}

std::optional<MessageReading> messages_reading(Language language, int floor, float x, float z) {
    return ext::reading_now(language, floor, x, z);
}

#else

std::vector<GroundMark> messages_marks(int) {
    return {};
}

bool messages_action(int, float, float, float) {
    return false;
}

std::optional<MessageReading> messages_reading(Language, int, float, float) {
    return std::nullopt;
}

#endif

} // namespace kf
