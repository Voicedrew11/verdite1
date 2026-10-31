#pragma once
// KF1's messages on the ground (docs/MESSAGES_PLAN.md): a template with up to two blanks, each
// filled by a word. On the wire a message is 5 bytes, the record's payload: the template, then
// two word ids, 0xffff for none. Template and word ids only ever grow at the end of their tables,
// so a message posted by any build reads the same in every later one, in either language.
#include <kf/platform/language.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace kf::ext {

inline constexpr std::uint16_t message_no_word = 0xffff;
inline constexpr std::size_t message_payload_bytes = 5;

struct Message {
    std::uint8_t template_id = 0;
    std::array<std::uint16_t, 2> words{message_no_word, message_no_word};
    bool operator==(const Message &) const = default;
};

// What a word is, for the writer's first list; a word id's top nibble says which table it is in.
enum class MessageWordKind : std::uint8_t { Direction, Feature, Action, Enemy, Person, Condition, Item };

struct MessageTemplate {
    std::string_view english, japanese;   // "___" marks each blank, in order
    std::uint8_t blanks = 1;              // 0, 1 or 2
};
std::span<const MessageTemplate> message_templates();

struct MessageWord {
    std::uint16_t id = 0;
    MessageWordKind kind = MessageWordKind::Direction;
};
// The words a writer can choose, of one kind, in the table's order. Items are those the game
// names (key items and equipment included), read from the disc in the drawn language.
std::span<const MessageWord> message_words(MessageWordKind kind);

std::array<std::uint8_t, message_payload_bytes> message_encode(const Message &message);
// A payload from the network: none when its size is wrong, its template is unknown, or a word is
// unknown or missing where its template has a blank (or present where it has none).
std::optional<Message> message_decode(std::span<const std::uint8_t> payload);

// A piece of a message as drawn: text menu_spell spells (English capitals, or full-size kana),
// or an item's name as the menus draw it from the disc (kf/game/menu.h's items rows).
inline constexpr std::uint8_t message_no_item = 0xff;
struct MessagePiece {
    std::string text;                      // empty when item is set
    std::uint8_t item = message_no_item;
    bool operator==(const MessagePiece &) const = default;
};
// The message in language's words, in order, as the pickup feed draws a line. Assumes a decoded
// message.
std::vector<MessagePiece> message_pieces(const Message &message, Language language);
// The word alone, as the writer lists it.
MessagePiece message_word_piece(std::uint16_t word, Language language);

} // namespace kf::ext
