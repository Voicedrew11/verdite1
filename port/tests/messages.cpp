#include "../ext/messages.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace kf;
using namespace kf::ext;

namespace {

constexpr std::u32string_view kana_allowed =
    U"あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよわんらりるれろ"
    U"がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ"
    U"アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨワンラリルレロ"
    U"ガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポヴ"
    U"ー ";

std::optional<std::size_t> utf8_length(unsigned char lead) {
    if (lead < 0x80)
        return 1;
    if (lead >= 0xc0 && lead < 0xe0)
        return 2;
    if (lead >= 0xe0 && lead < 0xf0)
        return 3;
    if (lead >= 0xf0 && lead < 0xf8)
        return 4;
    return std::nullopt;
}

// The text's characters, or none when it is not UTF-8.
std::optional<std::u32string> decode_utf8(std::string_view text) {
    std::u32string out;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        const std::optional<std::size_t> length = utf8_length(lead);
        if (!length || at + *length > text.size())
            return std::nullopt;
        char32_t code = *length == 1 ? lead : lead & (0xff >> (*length + 1));
        for (std::size_t k = 1; k < *length; ++k) {
            const auto next = static_cast<unsigned char>(text[at + k]);
            if ((next & 0xc0) != 0x80)
                return std::nullopt;
            code = (code << 6) | (next & 0x3f);
        }
        out.push_back(code);
        at += *length;
    }
    return out;
}

bool only_english(std::string_view text, std::string_view allowed) {
    return std::ranges::all_of(text, [allowed](char c) { return allowed.find(c) != std::string_view::npos; });
}

bool only_kana(std::string_view text, std::u32string_view allowed) {
    const std::optional<std::u32string> decoded = decode_utf8(text);
    if (!decoded)
        return false;
    return std::ranges::all_of(*decoded, [allowed](char32_t c) { return allowed.find(c) != std::u32string_view::npos; });
}

std::size_t count_blanks(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t at = text.find("___"); at != std::string_view::npos; at = text.find("___", at + 3))
        ++count;
    return count;
}

// Two word ids as a payload: the template, then each word little-endian.
std::vector<std::uint8_t> payload_of(std::uint8_t template_id, std::uint16_t first, std::uint16_t second) {
    return {template_id, static_cast<std::uint8_t>(first & 0xff), static_cast<std::uint8_t>(first >> 8),
            static_cast<std::uint8_t>(second & 0xff), static_cast<std::uint8_t>(second >> 8)};
}

std::uint16_t hand_written_id(MessageWordKind kind, std::size_t index) {
    return message_words(kind)[index].id;
}

constexpr std::uint16_t item_id_word(std::uint8_t item) {
    return static_cast<std::uint16_t>(0x1000 | item);
}

} // namespace

TEST_CASE("every template round-trips through encode and decode with words of each kind") {
    const auto templates = message_templates();
    for (std::size_t id = 0; id < templates.size(); ++id) {
        for (const MessageWordKind kind : {MessageWordKind::Direction, MessageWordKind::Feature,
                                           MessageWordKind::Enemy, MessageWordKind::Condition,
                                           MessageWordKind::Item}) {
            const auto words = message_words(kind);
            Message message;
            message.template_id = static_cast<std::uint8_t>(id);
            if (templates[id].blanks > 0)
                message.words[0] = words.back().id;
            if (templates[id].blanks > 1)
                message.words[1] = words.front().id;
            const auto payload = message_encode(message);
            CHECK(message_decode(payload) == std::optional<Message>{message});
        }
    }
}

TEST_CASE("a payload is the template, then the two words little-endian") {
    const Message message{12, {0x0000, 0x1000}};
    CHECK(message_encode(message) == std::array<std::uint8_t, 5>{12, 0x00, 0x00, 0x00, 0x10});
    const Message none{14, {message_no_word, message_no_word}};
    CHECK(message_encode(none) == std::array<std::uint8_t, 5>{14, 0xff, 0xff, 0xff, 0xff});
}

TEST_CASE("decode rejects a payload of the wrong size") {
    CHECK_FALSE(message_decode(std::vector<std::uint8_t>{}).has_value());
    CHECK_FALSE(message_decode(std::vector<std::uint8_t>{14, 0xff, 0xff, 0xff}).has_value());
    CHECK_FALSE(message_decode(std::vector<std::uint8_t>{14, 0xff, 0xff, 0xff, 0xff, 0}).has_value());
}

TEST_CASE("decode rejects an unknown template") {
    const auto unknown = static_cast<std::uint8_t>(message_templates().size());
    CHECK_FALSE(message_decode(payload_of(unknown, message_no_word, message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(255, message_no_word, message_no_word)).has_value());
}

TEST_CASE("decode rejects an unknown hand-written word or item") {
    // Template 0 has one blank, filled by a hand-written word or an item.
    const std::uint16_t after_life = static_cast<std::uint16_t>(message_words(MessageWordKind::Condition).back().id + 1);
    CHECK_FALSE(message_decode(payload_of(0, after_life, message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(0, 0xfffe, message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(0, item_id_word(12), message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(0, item_id_word(80), message_no_word)).has_value());
    CHECK(message_decode(payload_of(0, item_id_word(0), message_no_word)).has_value());
}

TEST_CASE("decode rejects a missing word where a blank is and an extra word where none is") {
    CHECK_FALSE(message_decode(payload_of(0, message_no_word, message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(12, 0, message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(14, 0, message_no_word)).has_value());
    CHECK_FALSE(message_decode(payload_of(15, message_no_word, 0)).has_value());
    CHECK(message_decode(payload_of(12, 0, item_id_word(1))).has_value());
    CHECK(message_decode(payload_of(14, message_no_word, message_no_word)).has_value());
}

TEST_CASE("the English text uses only capitals, digits, spaces and blanks") {
    for (const MessageTemplate &tmpl : message_templates())
        CHECK(only_english(tmpl.english, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _"));
    for (const MessageWordKind kind : {MessageWordKind::Direction, MessageWordKind::Feature,
                                       MessageWordKind::Action, MessageWordKind::Enemy,
                                       MessageWordKind::Person, MessageWordKind::Condition}) {
        for (const MessageWord &word : message_words(kind))
            CHECK(only_english(message_word_piece(word.id, Language::English).text,
                               "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "));
    }
}

TEST_CASE("the Japanese text uses only the font's kana, its long vowel mark, spaces and blanks") {
    const std::u32string allowed_in_templates = std::u32string(kana_allowed) + U"_";
    for (const MessageTemplate &tmpl : message_templates())
        CHECK(only_kana(tmpl.japanese, allowed_in_templates));
    for (const MessageWordKind kind : {MessageWordKind::Direction, MessageWordKind::Feature,
                                       MessageWordKind::Action, MessageWordKind::Enemy,
                                       MessageWordKind::Person, MessageWordKind::Condition}) {
        for (const MessageWord &word : message_words(kind))
            CHECK(only_kana(message_word_piece(word.id, Language::Japanese).text, kana_allowed));
    }
}

TEST_CASE("every template has as many blanks as it says in both languages") {
    for (const MessageTemplate &tmpl : message_templates()) {
        CHECK(count_blanks(tmpl.english) == tmpl.blanks);
        CHECK(count_blanks(tmpl.japanese) == tmpl.blanks);
    }
}

TEST_CASE("every hand-written word has its text in both languages") {
    for (const MessageWordKind kind : {MessageWordKind::Direction, MessageWordKind::Feature,
                                       MessageWordKind::Action, MessageWordKind::Enemy,
                                       MessageWordKind::Person, MessageWordKind::Condition}) {
        for (const MessageWord &word : message_words(kind)) {
            CHECK_FALSE(message_word_piece(word.id, Language::English).text.empty());
            CHECK_FALSE(message_word_piece(word.id, Language::Japanese).text.empty());
        }
    }
}

TEST_CASE("pieces of a two-blank template are the first word, the text between, then the second word") {
    const std::uint16_t left = hand_written_id(MessageWordKind::Direction, 0);
    const Message message{12, {left, item_id_word(0)}};
    CHECK(message_pieces(message, Language::English) ==
          std::vector<MessagePiece>{{"LEFT"}, {" AND "}, {"", 0}});
    CHECK(message_pieces(message, Language::Japanese) ==
          std::vector<MessagePiece>{{"ひだり"}, {" と "}, {"", 0}});
}

TEST_CASE("pieces of a template with one blank put the text around the word") {
    const Message message{0, {hand_written_id(MessageWordKind::Enemy, 1), message_no_word}};
    CHECK(message_pieces(message, Language::English) == std::vector<MessagePiece>{{"SLIME"}, {" AHEAD"}});
    CHECK(message_pieces(message, Language::Japanese) == std::vector<MessagePiece>{{"このさき "}, {"スライム"}});
}

TEST_CASE("pieces of a template with no blanks are its text") {
    const Message message{14, {message_no_word, message_no_word}};
    CHECK(message_pieces(message, Language::English) == std::vector<MessagePiece>{{"TURN BACK"}});
}

TEST_CASE("word ids are stable: the table is append-only") {
    CHECK(hand_written_id(MessageWordKind::Direction, 0) == 0x0000);
    CHECK(hand_written_id(MessageWordKind::Direction, 9) == 0x0009);
    CHECK(message_word_piece(0x0009, Language::English).text == "WEST");
    CHECK(message_word_piece(0x000a, Language::English).text == "HIDDEN WALL");
    CHECK(message_word_piece(0x0046, Language::English).text == "LIFE");
    CHECK(message_words(MessageWordKind::Condition).back().id == 0x0046);
    CHECK(message_word_piece(0x0047, Language::English) == MessagePiece{});
}

TEST_CASE("the item words are the named items, in id order") {
    const auto items = message_words(MessageWordKind::Item);
    CHECK(items.size() == 69);
    CHECK(items.front().id == 0x1000);
    CHECK(items.back().id == item_id_word(76));
    CHECK(std::ranges::is_sorted(items, std::ranges::less{}, &MessageWord::id));
    CHECK(message_word_piece(0x1000, Language::English) == MessagePiece{"", 0});
    CHECK(message_word_piece(0x100c, Language::English) == MessagePiece{});
}

TEST_CASE("the word lists hold their kind's words in table order") {
    const auto directions = message_words(MessageWordKind::Direction);
    CHECK(directions.size() == 10);
    CHECK(directions.front().id == 0x0000);
    CHECK(directions.back().id == 0x0009);
    for (const MessageWord &word : directions)
        CHECK(word.kind == MessageWordKind::Direction);
}
