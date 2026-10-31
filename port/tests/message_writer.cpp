#include "../ext/message_writer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace kf;
using namespace kf::ext;

namespace {

using Key = ItemMenuKey;
using Step = MessageWriter::Step;
using Pressed = MessageWriter::Pressed;

constexpr std::size_t rows = item_menu_visible_rows;
constexpr std::size_t two_blank_template = 12;   // "___ AND ___"
constexpr std::size_t no_blank_template = 15;    // "SAFE HERE"
constexpr std::size_t item_kind = 6;

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

// Presses key times over, with mine as Your messages; the last press's result.
Pressed go(MessageWriter &writer, Key key, std::size_t times = 1, std::span<const MessageOut> mine = {}) {
    Pressed last;
    for (std::size_t i = 0; i < times; ++i)
        last = writer.press(key, mine);
    return last;
}

// A line as the text it reads: an item shows as its number, a gap as its underscores.
std::string show(const ItemMenuLine &line) {
    std::string out;
    for (const MessageTextPiece &piece : line) {
        if (piece.item == message_text_only)
            out += piece.text;
        else
            out += "[item " + std::to_string(piece.item) + "]";
    }
    return out;
}

std::string show_footer(const ItemMenuPage &page) {
    return page.footer.empty() ? std::string("(none)") : show(page.footer.front());
}

std::vector<MessageOut> some_mine() {
    return {
        {Message{15}, 2},
        {Message{12, {0x0000, 0x1001}}, 0},
    };
}

// Every piece a menu draws as text, in order, for the font check.
void take_line(std::vector<std::string> &drawn, const ItemMenuLine &line) {
    for (const MessageTextPiece &piece : line) {
        if (piece.item == message_text_only && piece.text != item_menu_blank)
            drawn.push_back(piece.text);
    }
}

void take(std::vector<std::string> &drawn, const ItemMenuPage &page) {
    take_line(drawn, page.title);
    for (const ItemMenuLine &row : page.rows)
        take_line(drawn, row);
    for (const ItemMenuLine &line : page.footer)
        take_line(drawn, line);
}

// Walks every page the writer can show, in language, and returns the text it draws.
std::vector<std::string> drawn_by_walk(Language language) {
    std::vector<std::string> drawn;
    const std::vector<MessageOut> mine = some_mine();
    MessageWriter writer{rows};
    take(drawn, writer.page(language, {}));
    go(writer, Key::Down);
    take(drawn, writer.page(language, {}));
    go(writer, Key::Confirm);
    take(drawn, writer.page(language, {}));
    take(drawn, writer.page(language, mine));
    go(writer, Key::Back);
    go(writer, Key::Up);
    go(writer, Key::Confirm);
    for (std::size_t i = 0; i < message_templates().size(); ++i) {
        take(drawn, writer.page(language, {}));
        go(writer, Key::Down);
    }
    for (std::size_t kind = 0; kind < 7; ++kind) {
        MessageWriter path{rows};
        go(path, Key::Confirm);
        go(path, Key::Down, two_blank_template);
        go(path, Key::Confirm);
        go(path, Key::Down, kind);
        take(drawn, path.page(language, {}));
        go(path, Key::Confirm);
        const std::size_t words = message_words(static_cast<MessageWordKind>(kind)).size();
        for (std::size_t i = 0; i < words; ++i) {
            take(drawn, path.page(language, {}));
            go(path, Key::Down);
        }
        go(path, Key::Confirm);
        take(drawn, path.page(language, {}));
        go(path, Key::Confirm);
        take(drawn, path.page(language, {}));
        go(path, Key::Confirm);
        take(drawn, path.page(language, {}));
        go(path, Key::Confirm);
        take(drawn, path.page(language, {}));
        go(path, Key::Confirm);
        take(drawn, path.page(language, {}));
        go(path, Key::Down);
        take(drawn, path.page(language, {}));
    }
    return drawn;
}

} // namespace

TEST_CASE("the first page offers leaving a message and your messages, with no footer") {
    MessageWriter writer{rows};
    const ItemMenuPage page = writer.page(Language::English, {});
    CHECK(show(page.title) == "MESSAGE STONE");
    REQUIRE(page.rows.size() == 2);
    CHECK(show(page.rows[0]) == "LEAVE A MESSAGE");
    CHECK(show(page.rows[1]) == "YOUR MESSAGES");
    CHECK(page.cursor == 0);
    CHECK_FALSE(page.more_above);
    CHECK_FALSE(page.more_below);
    CHECK(page.footer.empty());
    const ItemMenuPage japanese = writer.page(Language::Japanese, {});
    CHECK(show(japanese.title) == "ことばの いし");
    CHECK(show(japanese.rows[0]) == "ことばの かきこみ");
    CHECK(show(japanese.rows[1]) == "のこした ことば");
}

TEST_CASE("Back on the first page closes the menu with no message") {
    MessageWriter writer{rows};
    const Pressed pressed = go(writer, Key::Back);
    CHECK(pressed.state == ItemMenuState::Closed);
    CHECK_FALSE(pressed.left.has_value());
}

TEST_CASE("Up and Down wrap at both ends of a list") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);   // Template, row 0
    go(writer, Key::Up);
    CHECK(writer.cursor() == message_templates().size() - 1);
    CHECK(writer.first_visible() == message_templates().size() - rows);
    CHECK(writer.page(Language::English, {}).cursor == rows - 1);
    go(writer, Key::Down);
    CHECK(writer.cursor() == 0);
    CHECK(writer.first_visible() == 0);
    CHECK(writer.page(Language::English, {}).more_below);
    CHECK_FALSE(writer.page(Language::English, {}).more_above);
}

TEST_CASE("a long list scrolls by the least that keeps the cursor among the visible rows") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);             // Template, row 0
    go(writer, Key::Confirm);             // template 0, one blank: Kind
    go(writer, Key::Down, item_kind);
    go(writer, Key::Confirm);             // Word, the items: 69 of them
    REQUIRE(message_words(MessageWordKind::Item).size() == 69);

    go(writer, Key::Down, 7);
    CHECK(writer.cursor() == 7);
    CHECK(writer.first_visible() == 1);
    ItemMenuPage page = writer.page(Language::English, {});
    CHECK(page.rows.size() == rows);
    CHECK(page.cursor == rows - 1);
    CHECK(page.more_above);
    CHECK(page.more_below);

    go(writer, Key::Down, 61);
    CHECK(writer.cursor() == 68);
    CHECK(writer.first_visible() == 62);
    page = writer.page(Language::English, {});
    CHECK(page.rows.size() == rows);
    CHECK_FALSE(page.more_below);

    go(writer, Key::Down);
    CHECK(writer.cursor() == 0);
    CHECK(writer.first_visible() == 0);
    go(writer, Key::Up);
    CHECK(writer.cursor() == 68);
    CHECK(writer.first_visible() == 62);
}

TEST_CASE("a template with no blanks goes straight to Confirm, which leaves its message") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Up);                  // wraps to the last template
    const Pressed chosen = go(writer, Key::Confirm);
    CHECK(chosen.state == ItemMenuState::Open);
    CHECK(writer.step() == Step::Confirm);
    CHECK(writer.blank() == 0);
    const ItemMenuPage page = writer.page(Language::English, {});
    CHECK(show(page.title) == "LEAVE IT HERE");
    CHECK(show_footer(page) == "SAFE HERE");
    const Pressed done = go(writer, Key::Confirm);
    CHECK(done.state == ItemMenuState::Done);
    CHECK(done.left == std::optional<Message>{Message{no_blank_template}});
}

TEST_CASE("a two-blank template is filled blank by blank into the message it leaves") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Down, two_blank_template);
    go(writer, Key::Confirm);
    REQUIRE(writer.step() == Step::Kind);
    CHECK(writer.blank() == 0);
    CHECK(show_footer(writer.page(Language::English, {})) == "___ AND ___");

    go(writer, Key::Confirm);             // Direction
    REQUIRE(writer.step() == Step::Word);
    go(writer, Key::Confirm);             // LEFT
    REQUIRE(writer.step() == Step::Kind);
    CHECK(writer.blank() == 1);
    CHECK(show_footer(writer.page(Language::English, {})) == "LEFT AND ___");

    go(writer, Key::Down, item_kind);
    go(writer, Key::Confirm);             // Word for blank 1
    REQUIRE(writer.step() == Step::Word);
    go(writer, Key::Down);                // item 1
    go(writer, Key::Confirm);
    REQUIRE(writer.step() == Step::Confirm);
    const ItemMenuPage page = writer.page(Language::English, {});
    CHECK(show_footer(page) == "LEFT AND [item 1]");

    const Pressed done = go(writer, Key::Confirm);
    CHECK(done.state == ItemMenuState::Done);
    CHECK(done.left == std::optional<Message>{Message{two_blank_template, {0x0000, 0x1001}}});
}

TEST_CASE("the template rows show each blank as a gap, and the Kind footer keeps the gap") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Down, two_blank_template);
    const ItemMenuPage templates = writer.page(Language::English, {});
    CHECK(templates.cursor == 6);
    CHECK(show(templates.rows[templates.cursor]) == "___ AND ___");
    CHECK(show(templates.rows[0]) == "___ LIES BEYOND");
}

TEST_CASE("choosing a template resets the draft") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Down, two_blank_template);
    go(writer, Key::Confirm);
    go(writer, Key::Confirm);             // Direction
    go(writer, Key::Confirm);             // LEFT for blank 0
    REQUIRE(writer.draft().words[0] == 0x0000);
    go(writer, Key::Back);                // Word for blank 0, cleared
    go(writer, Key::Back);                // Kind for blank 0
    go(writer, Key::Back);                // Template, row 12
    go(writer, Key::Up);                  // template 11, "SEARCH ___"
    go(writer, Key::Confirm);
    CHECK(writer.draft().template_id == 11);
    CHECK(writer.draft().words[0] == message_no_word);
    CHECK(writer.draft().words[1] == message_no_word);
}

TEST_CASE("Back from each page returns to the one before, with the row chosen there") {
    MessageWriter writer{rows};
    go(writer, Key::Down);
    go(writer, Key::Confirm);             // Mine
    REQUIRE(writer.step() == Step::Mine);
    go(writer, Key::Back);
    CHECK(writer.step() == Step::First);
    CHECK(writer.cursor() == 1);

    go(writer, Key::Up);
    go(writer, Key::Confirm);             // Template, row 0
    go(writer, Key::Back);
    CHECK(writer.step() == Step::First);
    CHECK(writer.cursor() == 0);

    go(writer, Key::Confirm);             // Template
    go(writer, Key::Down, 5);
    go(writer, Key::Confirm);             // Kind for blank 0 of template 5
    go(writer, Key::Back);
    CHECK(writer.step() == Step::Template);
    CHECK(writer.cursor() == 5);

    go(writer, Key::Confirm);
    go(writer, Key::Down, 2);
    go(writer, Key::Confirm);             // Word for blank 0, Action
    go(writer, Key::Down, 3);
    go(writer, Key::Back);
    CHECK(writer.step() == Step::Kind);
    CHECK(writer.cursor() == 2);
    CHECK(writer.draft().words[0] == message_no_word);
    go(writer, Key::Back);
    CHECK(writer.step() == Step::Template);
    CHECK(writer.cursor() == 5);
}

TEST_CASE("Back from the second blank's kind returns to the first blank's word, cleared, at its row") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Down, two_blank_template);
    go(writer, Key::Confirm);
    go(writer, Key::Down);                // Feature
    go(writer, Key::Confirm);
    go(writer, Key::Down, 4);             // PIT
    go(writer, Key::Confirm);
    REQUIRE(writer.step() == Step::Kind);
    REQUIRE(writer.blank() == 1);
    go(writer, Key::Down, item_kind);
    go(writer, Key::Confirm);
    go(writer, Key::Down, 2);
    go(writer, Key::Back);                // Kind for blank 1, at Item
    CHECK(writer.step() == Step::Kind);
    CHECK(writer.blank() == 1);
    CHECK(writer.cursor() == item_kind);
    CHECK(writer.draft().words[1] == message_no_word);

    go(writer, Key::Back);                // Word for blank 0
    CHECK(writer.step() == Step::Word);
    CHECK(writer.blank() == 0);
    CHECK(writer.draft().words[0] == message_no_word);
    const ItemMenuPage page = writer.page(Language::English, {});
    CHECK(page.cursor == 4);
    CHECK(show(page.rows[page.cursor]) == "PIT");
}

TEST_CASE("Back from Confirm returns to the last blank's word at its row; with no blanks, to the templates") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Down, two_blank_template);
    go(writer, Key::Confirm);
    go(writer, Key::Confirm);             // Direction
    go(writer, Key::Confirm);             // LEFT
    go(writer, Key::Down, 2);             // Action
    go(writer, Key::Confirm);
    go(writer, Key::Down, 3);
    go(writer, Key::Confirm);             // PATIENCE for blank 1
    REQUIRE(writer.step() == Step::Confirm);

    go(writer, Key::Down);                // Back, through the second row
    const Pressed back = go(writer, Key::Confirm);
    CHECK(back.state == ItemMenuState::Open);
    CHECK(writer.step() == Step::Word);
    CHECK(writer.blank() == 1);
    CHECK(writer.cursor() == 3);
    CHECK(writer.draft().words[1] == message_no_word);
    CHECK(writer.draft().words[0] == 0x0000);

    go(writer, Key::Back);
    CHECK(writer.step() == Step::Kind);
    CHECK(writer.cursor() == 2);

    MessageWriter none{rows};
    go(none, Key::Confirm);
    go(none, Key::Down, no_blank_template);
    go(none, Key::Confirm);
    REQUIRE(none.step() == Step::Confirm);
    go(none, Key::Back);
    CHECK(none.step() == Step::Template);
    CHECK(none.cursor() == no_blank_template);
}

TEST_CASE("Your messages lists each message with its good count, and NONE when there are none") {
    MessageWriter writer{rows};
    go(writer, Key::Down);
    go(writer, Key::Confirm);             // Mine
    const ItemMenuPage empty = writer.page(Language::English, {});
    CHECK(show(empty.title) == "YOUR MESSAGES");
    REQUIRE(empty.rows.size() == 1);
    CHECK(show(empty.rows[0]) == "NONE");
    CHECK(empty.cursor == 1);
    CHECK(empty.footer.empty());
    CHECK(show(writer.page(Language::Japanese, {}).rows[0]) == "なし");

    const std::vector<MessageOut> mine = some_mine();
    const ItemMenuPage page = writer.page(Language::English, mine);
    REQUIRE(page.rows.size() == 2);
    CHECK(show(page.rows[0]) == "SAFE HERE GOOD 2");
    CHECK(show(page.rows[1]) == "LEFT AND [item 1] GOOD 0");
    CHECK(page.cursor == 2);
    CHECK(page.footer.empty());
    CHECK(show(writer.page(Language::Japanese, mine).rows[0]) == "ここは あんぜん よい 2");
}

TEST_CASE("Confirm does nothing on Your messages, and Back returns to the first page") {
    MessageWriter writer{rows};
    go(writer, Key::Down);
    go(writer, Key::Confirm);
    const Pressed pressed = go(writer, Key::Confirm);
    CHECK(pressed.state == ItemMenuState::Open);
    CHECK_FALSE(pressed.left.has_value());
    CHECK(writer.step() == Step::Mine);
    go(writer, Key::Back);
    CHECK(writer.step() == Step::First);
    CHECK(writer.cursor() == 1);
}

TEST_CASE("Up and Down scroll Your messages, and stop at its ends") {
    std::vector<MessageOut> mine(10, MessageOut{Message{15}, 0});
    for (std::size_t i = 0; i < mine.size(); ++i)
        mine[i].good = static_cast<std::uint32_t>(i);
    MessageWriter writer{rows};
    go(writer, Key::Down);
    go(writer, Key::Confirm);             // Mine
    go(writer, Key::Up, 1, mine);
    CHECK(writer.first_visible() == 0);
    go(writer, Key::Down, 5, mine);
    CHECK(writer.first_visible() == mine.size() - rows);
    const ItemMenuPage page = writer.page(Language::English, mine);
    CHECK(show(page.rows.back()) == "SAFE HERE GOOD 9");
    CHECK(page.more_above);
    CHECK_FALSE(page.more_below);
    go(writer, Key::Up, 1, mine);
    CHECK(writer.first_visible() == mine.size() - rows - 1);
    go(writer, Key::Down, 1, std::span<const MessageOut>(mine).first(2));
    CHECK(writer.first_visible() == mine.size() - rows - 1);   // a short list does not scroll on
    CHECK(writer.page(Language::English, std::span<const MessageOut>(mine).first(2)).rows.size() == 2);
}

TEST_CASE("Confirm's second row is Back, from the Confirm page") {
    MessageWriter writer{rows};
    go(writer, Key::Confirm);
    go(writer, Key::Confirm);             // template 0 (one blank)
    go(writer, Key::Confirm);             // Direction
    go(writer, Key::Confirm);             // LEFT
    REQUIRE(writer.step() == Step::Confirm);
    const ItemMenuPage page = writer.page(Language::English, {});
    CHECK(show(page.rows[1]) == "BACK");
    const Pressed back = go(writer, Key::Down);
    CHECK(back.state == ItemMenuState::Open);
    const Pressed pressed = go(writer, Key::Confirm);
    CHECK(pressed.state == ItemMenuState::Open);
    CHECK_FALSE(pressed.left.has_value());
    CHECK(writer.step() == Step::Word);
    CHECK(writer.blank() == 0);
    CHECK(writer.draft().words[0] == message_no_word);
}

TEST_CASE("everything the writer draws is in the menu font, in both languages") {
    for (const Language language : {Language::English, Language::Japanese}) {
        for (const std::string &text : drawn_by_walk(language)) {
            if (language == Language::English) {
                CHECK(only_english(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 "));
            } else {
                CHECK(only_kana(text, std::u32string(kana_allowed) + U"0123456789"));
            }
        }
    }
}
