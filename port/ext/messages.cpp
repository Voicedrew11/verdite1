#include "messages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace kf::ext {
namespace {

constexpr std::uint16_t item_word_base = 0x1000;
constexpr std::size_t kind_count = static_cast<std::size_t>(MessageWordKind::Item) + 1;
constexpr std::string_view blank_mark = "___";

struct HandWrittenWord {
    MessageWordKind kind;
    std::string_view english, japanese;
};

// Append-only: a word's id is its row, and ids are on the wire (docs/MESSAGES_PLAN.md).
constexpr auto hand_written_words = std::to_array<HandWrittenWord>({
    {MessageWordKind::Direction, "LEFT", "ひだり"},
    {MessageWordKind::Direction, "RIGHT", "みぎ"},
    {MessageWordKind::Direction, "BEHIND", "うしろ"},
    {MessageWordKind::Direction, "ABOVE", "うえ"},
    {MessageWordKind::Direction, "BELOW", "した"},
    {MessageWordKind::Direction, "AHEAD", "まえ"},
    {MessageWordKind::Direction, "NORTH", "きた"},
    {MessageWordKind::Direction, "SOUTH", "みなみ"},
    {MessageWordKind::Direction, "EAST", "ひがし"},
    {MessageWordKind::Direction, "WEST", "にし"},
    {MessageWordKind::Feature, "HIDDEN WALL", "かくしかべ"},
    {MessageWordKind::Feature, "HIDDEN DOOR", "かくしとびら"},
    {MessageWordKind::Feature, "DOOR", "とびら"},
    {MessageWordKind::Feature, "KEY", "かぎ"},
    {MessageWordKind::Feature, "PIT", "おとしあな"},
    {MessageWordKind::Feature, "TRAP", "わな"},
    {MessageWordKind::Feature, "LEVER", "レバー"},
    {MessageWordKind::Feature, "STAIRS", "かいだん"},
    {MessageWordKind::Feature, "WATER", "みず"},
    {MessageWordKind::Feature, "LAVA", "ようがん"},
    {MessageWordKind::Feature, "POISON SWAMP", "どくのぬま"},
    {MessageWordKind::Feature, "SHORTCUT", "ちかみち"},
    {MessageWordKind::Feature, "DEAD END", "いきどまり"},
    {MessageWordKind::Feature, "CHEST", "たからばこ"},
    {MessageWordKind::Feature, "SPRING", "いずみ"},
    {MessageWordKind::Feature, "EXIT", "でぐち"},
    {MessageWordKind::Feature, "LADDER", "はしご"},
    {MessageWordKind::Feature, "WALL", "かべ"},
    {MessageWordKind::Action, "CHARGED ATTACK", "ためこうげき"},
    {MessageWordKind::Action, "MAGIC", "まほう"},
    {MessageWordKind::Action, "RUNNING AWAY", "にげること"},
    {MessageWordKind::Action, "PATIENCE", "がまん"},
    {MessageWordKind::Action, "SEARCHING", "たんさく"},
    {MessageWordKind::Action, "RESTING", "やすむこと"},
    {MessageWordKind::Action, "FIGHTING", "たたかい"},
    {MessageWordKind::Action, "SNEAKING", "しのびあし"},
    {MessageWordKind::Action, "CLOSING IN", "ちかづくこと"},
    {MessageWordKind::Enemy, "SKELETON", "がいこつ"},
    {MessageWordKind::Enemy, "SLIME", "スライム"},
    {MessageWordKind::Enemy, "GHOST", "ゆうれい"},
    {MessageWordKind::Enemy, "DRAGON", "ドラゴン"},
    {MessageWordKind::Enemy, "BAT", "こうもり"},
    {MessageWordKind::Enemy, "ZOMBIE", "ゾンビ"},
    {MessageWordKind::Enemy, "KNIGHT", "きし"},
    {MessageWordKind::Enemy, "MONSTER", "まもの"},
    {MessageWordKind::Enemy, "STRONG FOE", "つよいてき"},
    {MessageWordKind::Enemy, "MANY FOES", "たくさんのてき"},
    {MessageWordKind::Enemy, "AMBUSH", "まちぶせ"},
    {MessageWordKind::Person, "MERCHANT", "あきんど"},
    {MessageWordKind::Person, "FRIEND", "なかま"},
    {MessageWordKind::Person, "LIAR", "うそつき"},
    {MessageWordKind::Person, "VILLAGER", "むらびと"},
    {MessageWordKind::Person, "WIZARD", "まほうつかい"},
    {MessageWordKind::Person, "YOU", "あなた"},
    {MessageWordKind::Person, "ME", "わたし"},
    {MessageWordKind::Condition, "POISON", "どく"},
    {MessageWordKind::Condition, "CURSE", "のろい"},
    {MessageWordKind::Condition, "DARKNESS", "やみ"},
    {MessageWordKind::Condition, "SLOWNESS", "どんそく"},
    {MessageWordKind::Condition, "DEATH", "し"},
    {MessageWordKind::Condition, "HEALING", "かいふく"},
    {MessageWordKind::Condition, "DANGER", "きけん"},
    {MessageWordKind::Condition, "SAFETY", "あんぜん"},
    {MessageWordKind::Condition, "TREASURE", "たから"},
    {MessageWordKind::Condition, "GOLD", "ゴールド"},
    {MessageWordKind::Condition, "LIGHT", "ひかり"},
    {MessageWordKind::Condition, "SECRET", "ひみつ"},
    {MessageWordKind::Condition, "LIES", "うそ"},
    {MessageWordKind::Condition, "HOPE", "きぼう"},
    {MessageWordKind::Condition, "DESPAIR", "ぜつぼう"},
    {MessageWordKind::Condition, "LIFE", "いのち"},
});
static_assert(hand_written_words.size() < item_word_base, "hand-written ids stay below the items'");

// Append-only, as the words are: a template's id is its row.
constexpr auto templates = std::to_array<MessageTemplate>({
    {"___ AHEAD", "このさき ___", 1},
    {"BEWARE OF ___", "___ に ようじんせよ", 1},
    {"TRY ___", "___ が よい", 1},
    {"HIDDEN ___ HERE", "ここに かくされた ___", 1},
    {"___ NEEDED", "___ が ひつよう", 1},
    {"DO NOT TRUST ___", "___ は しんじるな", 1},
    {"___ LIES BEYOND", "このむこうに ___", 1},
    {"WEAK TO ___", "___ に よわい", 1},
    {"___ IS DANGEROUS", "___ は きけん", 1},
    {"GO ___", "___ へ すすめ", 1},
    {"NO ___ HERE", "ここに ___ は ない", 1},
    {"SEARCH ___", "___ も しらべろ", 1},
    {"___ AND ___", "___ と ___", 2},
    {"IF ___ THEN ___", "___ なら ___", 2},
    {"TURN BACK", "ひきかえせ", 0},
    {"SAFE HERE", "ここは あんぜん", 0},
});
static_assert(templates.size() <= 256, "a template id is one byte");

// The item ids the game names: the non-null rows of automap.cpp's item_names, in order.
constexpr auto named_items = std::to_array<std::uint8_t>({
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    13, 14, 15, 16, 17,
    19, 20, 21, 22, 23, 24,
    26, 27, 28, 29, 30,
    32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    67, 68, 69, 70,
    73, 74, 75, 76,
});

constexpr std::uint16_t item_word(std::uint8_t item) {
    return static_cast<std::uint16_t>(item_word_base | item);
}

constexpr bool is_item_word(std::uint16_t word) {
    return word >= item_word_base && word < item_word_base + 0x100;
}

constexpr std::uint8_t item_of(std::uint16_t item_word_id) {
    return static_cast<std::uint8_t>(item_word_id - item_word_base);
}

bool is_named_item(std::uint8_t item) {
    return std::ranges::binary_search(named_items, item);
}

bool word_known(std::uint16_t word) {
    if (is_item_word(word))
        return is_named_item(item_of(word));
    return word < hand_written_words.size();
}

constexpr std::size_t kind_index(MessageWordKind kind) {
    return static_cast<std::size_t>(kind);
}

using WordLists = std::array<std::vector<MessageWord>, kind_count>;

WordLists build_word_lists() {
    WordLists lists;
    for (std::size_t id = 0; id < hand_written_words.size(); ++id) {
        const MessageWordKind kind = hand_written_words[id].kind;
        lists[kind_index(kind)].push_back({static_cast<std::uint16_t>(id), kind});
    }
    for (const std::uint8_t item : named_items)
        lists[kind_index(MessageWordKind::Item)].push_back({item_word(item), MessageWordKind::Item});
    return lists;
}

const WordLists &word_lists() {
    static const WordLists lists = build_word_lists();
    return lists;
}

std::string_view text_in(std::string_view english, std::string_view japanese, Language language) {
    return language == Language::English ? english : japanese;
}

std::array<std::uint8_t, 2> word_bytes(std::uint16_t word) {
    return {static_cast<std::uint8_t>(word & 0xff), static_cast<std::uint8_t>(word >> 8)};
}

// The little-endian word at offset, in a payload already checked to hold it.
std::uint16_t word_at(std::span<const std::uint8_t> payload, std::size_t offset) {
    return static_cast<std::uint16_t>(payload[offset] | (payload[offset + 1] << 8));
}

} // namespace

std::span<const MessageTemplate> message_templates() {
    return templates;
}

std::span<const MessageWord> message_words(MessageWordKind kind) {
    return word_lists()[kind_index(kind)];
}

std::array<std::uint8_t, message_payload_bytes> message_encode(const Message &message) {
    const std::array<std::uint8_t, 2> first = word_bytes(message.words[0]);
    const std::array<std::uint8_t, 2> second = word_bytes(message.words[1]);
    return {message.template_id, first[0], first[1], second[0], second[1]};
}

std::optional<Message> message_decode(std::span<const std::uint8_t> payload) {
    if (payload.size() != message_payload_bytes)
        return std::nullopt;
    const std::uint8_t template_id = payload[0];
    if (template_id >= templates.size())
        return std::nullopt;
    Message message;
    message.template_id = template_id;
    for (std::size_t blank = 0; blank < message.words.size(); ++blank) {
        const std::uint16_t word = word_at(payload, 1 + 2 * blank);
        const bool wanted = blank < templates[template_id].blanks;
        const bool present = word != message_no_word;
        if (wanted != present || (present && !word_known(word)))
            return std::nullopt;
        message.words[blank] = word;
    }
    return message;
}

MessagePiece message_word_piece(std::uint16_t word, Language language) {
    if (is_item_word(word) && is_named_item(item_of(word)))
        return {"", item_of(word)};
    if (word < hand_written_words.size()) {
        const HandWrittenWord &hand = hand_written_words[word];
        return {std::string(text_in(hand.english, hand.japanese, language))};
    }
    return {};
}

std::vector<MessagePiece> message_pieces(const Message &message, Language language) {
    std::vector<MessagePiece> pieces;
    if (message.template_id >= templates.size())
        return pieces;
    const MessageTemplate &tmpl = templates[message.template_id];
    std::string_view rest = text_in(tmpl.english, tmpl.japanese, language);
    std::size_t blank = 0;
    while (true) {
        const std::size_t at = rest.find(blank_mark);
        const std::string_view text = rest.substr(0, at);
        if (!text.empty())
            pieces.push_back({std::string(text)});
        if (at == std::string_view::npos || blank == message.words.size())
            return pieces;
        pieces.push_back(message_word_piece(message.words[blank], language));
        ++blank;
        rest.remove_prefix(at + blank_mark.size());
    }
}

} // namespace kf::ext
