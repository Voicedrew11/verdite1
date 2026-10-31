#include "message_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kf::ext {
namespace {

using Step = MessageWriter::Step;
using Pressed = MessageWriter::Pressed;

// The menu's text, in the game's font: no small kana, no を, no punctuation, no underscore.
struct Text {
    std::string_view english, japanese;
};

constexpr Text first_title{"MESSAGE STONE", "ことばの いし"};
constexpr Text leave_row{"LEAVE A MESSAGE", "ことばの かきこみ"};
constexpr Text mine_row{"YOUR MESSAGES", "のこした ことば"};
constexpr Text template_title{"CHOOSE A MESSAGE", "ぶんれい"};
constexpr Text kind_title{"CHOOSE A KIND", "ことばの なかま"};
constexpr Text word_title{"CHOOSE A WORD", "ことば"};
constexpr Text leave_here{"LEAVE IT HERE", "ここに おく"};
constexpr Text back_row{"BACK", "もどる"};
constexpr Text none_row{"NONE", "なし"};
constexpr Text good_label{" GOOD ", " よい "};

// In MessageWordKind's order.
constexpr std::size_t kind_count = static_cast<std::size_t>(MessageWordKind::Item) + 1;
constexpr auto kind_names = std::to_array<Text>({
    {"DIRECTION", "ほうこう"},
    {"FEATURE", "しかけ"},
    {"ACTION", "うごき"},
    {"ENEMY", "てき"},
    {"PERSON", "ひと"},
    {"CONDITION", "ようす"},
    {"ITEM", "どうぐ"},
});
static_assert(kind_names.size() == kind_count);

// The Confirm page's first row leaves the message; its second is Back. First's Mine row:
constexpr std::size_t leave_index = 0;
constexpr std::size_t first_mine_index = 1;
constexpr std::size_t first_list_size = 2;

constexpr std::string_view template_blank = "___";

// The writer's state, borrowed from the object so the steps are plain functions.
struct Stone {
    Step &step;
    std::size_t &cursor;
    std::size_t &first_visible;
    Message &draft;
    std::size_t &blank;
    std::size_t &kind;
    std::size_t visible_rows;
    std::size_t mine_count;
};

// Where a word is in the writer's lists: its kind, and its row in that kind's list.
struct WordPlace {
    std::size_t kind = 0;
    std::size_t row = 0;
};

std::span<const MessageWord> words_of(std::size_t kind) {
    return message_words(static_cast<MessageWordKind>(kind));
}

// The draft only holds words the lists hold, so a word not found is never asked for.
WordPlace word_place(std::uint16_t word) {
    for (std::size_t kind = 0; kind < kind_count; ++kind) {
        const auto words = words_of(kind);
        for (std::size_t row = 0; row < words.size(); ++row) {
            if (words[row].id == word)
                return {kind, row};
        }
    }
    return {};
}

std::size_t blanks_of(std::uint8_t template_id) {
    return message_templates()[template_id].blanks;
}

std::string_view text_in(std::string_view english, std::string_view japanese, Language language) {
    return language == Language::English ? english : japanese;
}

std::string_view text_in(Text text, Language language) {
    return text_in(text.english, text.japanese, language);
}

MessageTextPiece text_piece(std::string_view text) {
    return {std::string(text), message_text_only};
}

ItemMenuLine text_line(Text text, Language language) {
    return {text_piece(text_in(text, language))};
}

MessageTextPiece to_text_piece(const MessagePiece &piece) {
    return {piece.text, piece.item};
}

// The blank's piece: the word chosen for it, or a gap still to fill.
MessageTextPiece blank_piece(const Message &message, std::size_t blank, Language language) {
    const bool chosen = blank < message.words.size() && message.words[blank] != message_no_word;
    if (!chosen)
        return {std::string(item_menu_blank), message_text_only};
    return to_text_piece(message_word_piece(message.words[blank], language));
}

// The message as it reads so far: the template's text, each blank its word or a gap.
ItemMenuLine preview_line(const Message &message, Language language) {
    ItemMenuLine line;
    const auto templates = message_templates();
    if (message.template_id >= templates.size())
        return line;
    const MessageTemplate &tmpl = templates[message.template_id];
    std::string_view rest = text_in(tmpl.english, tmpl.japanese, language);
    for (std::size_t blank = 0;; ++blank) {
        const std::size_t at = rest.find(template_blank);
        const std::string_view text = rest.substr(0, at);
        if (!text.empty())
            line.push_back(text_piece(text));
        if (at == std::string_view::npos)
            return line;
        line.push_back(blank_piece(message, blank, language));
        rest.remove_prefix(at + template_blank.size());
    }
}

ItemMenuLine mine_line(const MessageOut &out, Language language) {
    ItemMenuLine line;
    for (const MessagePiece &piece : message_pieces(out.message, language))
        line.push_back(to_text_piece(piece));
    const std::string count = std::string(text_in(good_label, language)) + std::to_string(out.good);
    line.push_back({count, message_text_only});
    return line;
}

Text title_of(Step step) {
    switch (step) {
    case Step::First: return first_title;
    case Step::Mine: return mine_row;
    case Step::Template: return template_title;
    case Step::Kind: return kind_title;
    case Step::Word: return word_title;
    case Step::Confirm: return leave_here;
    }
    return first_title;
}

std::vector<ItemMenuLine> template_rows(Language language) {
    const auto templates = message_templates();
    std::vector<ItemMenuLine> rows;
    rows.reserve(templates.size());
    for (std::size_t id = 0; id < templates.size(); ++id) {
        Message message;
        message.template_id = static_cast<std::uint8_t>(id);
        rows.push_back(preview_line(message, language));
    }
    return rows;
}

std::vector<ItemMenuLine> kind_rows(Language language) {
    std::vector<ItemMenuLine> rows;
    rows.reserve(kind_names.size());
    for (const Text &name : kind_names)
        rows.push_back(text_line(name, language));
    return rows;
}

std::vector<ItemMenuLine> word_rows(std::size_t kind, Language language) {
    const auto words = words_of(kind);
    std::vector<ItemMenuLine> rows;
    rows.reserve(words.size());
    for (const MessageWord &word : words)
        rows.push_back({to_text_piece(message_word_piece(word.id, language))});
    return rows;
}

std::vector<ItemMenuLine> mine_rows(std::span<const MessageOut> mine, Language language) {
    if (mine.empty())
        return {text_line(none_row, language)};
    std::vector<ItemMenuLine> rows;
    rows.reserve(mine.size());
    for (const MessageOut &out : mine)
        rows.push_back(mine_line(out, language));
    return rows;
}

// Every row of the step's whole list, before the page shows its visible slice.
std::vector<ItemMenuLine> rows_of(Step step, std::size_t kind, Language language,
                                  std::span<const MessageOut> mine) {
    switch (step) {
    case Step::First:
        return {text_line(leave_row, language), text_line(mine_row, language)};
    case Step::Mine: return mine_rows(mine, language);
    case Step::Template: return template_rows(language);
    case Step::Kind: return kind_rows(language);
    case Step::Word: return word_rows(kind, language);
    case Step::Confirm:
        return {text_line(leave_here, language), text_line(back_row, language)};
    }
    return {};
}

// Kind, Word and Confirm show the draft under the rows.
std::vector<ItemMenuLine> footer_of(Step step, const Message &draft, Language language) {
    switch (step) {
    case Step::Kind:
    case Step::Word:
    case Step::Confirm:
        return {preview_line(draft, language)};
    case Step::First:
    case Step::Mine:
    case Step::Template:
        return {};
    }
    return {};
}

// The number of rows a list step has; Mine's rows scroll and are not counted here.
std::size_t list_size(Step step, std::size_t kind) {
    switch (step) {
    case Step::First:
    case Step::Confirm:
        return first_list_size;
    case Step::Template:
        return message_templates().size();
    case Step::Kind:
        return kind_count;
    case Step::Word:
        return words_of(kind).size();
    case Step::Mine:
        return 0;
    }
    return 0;
}

// The cursor on row, scrolled by the least that keeps it among the visible rows.
void place(Stone &stone, std::size_t row) {
    stone.cursor = row;
    if (row < stone.first_visible)
        stone.first_visible = row;
    else if (row >= stone.first_visible + stone.visible_rows)
        stone.first_visible = row + 1 - stone.visible_rows;
}

// A page is entered with its first row showing and the cursor on row.
void enter(Stone &stone, Step step, std::size_t row) {
    stone.step = step;
    stone.first_visible = 0;
    place(stone, row);
}

// Your messages has no cursor: Up and Down move its first visible row.
void scroll_mine(Stone &stone, ItemMenuKey key) {
    const std::size_t last_first = stone.mine_count > stone.visible_rows ? stone.mine_count - stone.visible_rows : 0;
    if (key == ItemMenuKey::Up && stone.first_visible > 0)
        stone.first_visible -= 1;
    else if (key == ItemMenuKey::Down && stone.first_visible < last_first)
        stone.first_visible += 1;
}

void cursor_down(Stone &stone) {
    const std::size_t count = list_size(stone.step, stone.kind);
    if (count == 0)
        return;
    place(stone, stone.cursor + 1 < count ? stone.cursor + 1 : 0);
}

void cursor_up(Stone &stone) {
    const std::size_t count = list_size(stone.step, stone.kind);
    if (count == 0)
        return;
    place(stone, stone.cursor == 0 ? count - 1 : stone.cursor - 1);
}

void choose_first(Stone &stone) {
    enter(stone, stone.cursor == 0 ? Step::Template : Step::Mine, 0);
}

void choose_template(Stone &stone) {
    stone.draft = Message{};
    stone.draft.template_id = static_cast<std::uint8_t>(stone.cursor);
    if (blanks_of(stone.draft.template_id) == 0) {
        enter(stone, Step::Confirm, 0);
        return;
    }
    stone.blank = 0;
    enter(stone, Step::Kind, 0);
}

void choose_kind(Stone &stone) {
    stone.kind = stone.cursor;
    enter(stone, Step::Word, 0);
}

void choose_word(Stone &stone) {
    const auto words = words_of(stone.kind);
    stone.draft.words[stone.blank] = words[stone.cursor].id;
    if (stone.blank + 1 < blanks_of(stone.draft.template_id)) {
        stone.blank += 1;
        enter(stone, Step::Kind, 0);
        return;
    }
    enter(stone, Step::Confirm, 0);
}

void back_to_template(Stone &stone) {
    enter(stone, Step::Template, stone.draft.template_id);
}

// Returns to the word for blank, with the row it was chosen at; its word is cleared.
void reopen_word(Stone &stone, std::size_t blank) {
    const WordPlace place_of_word = word_place(stone.draft.words[blank]);
    stone.blank = blank;
    stone.kind = place_of_word.kind;
    stone.draft.words[blank] = message_no_word;
    enter(stone, Step::Word, place_of_word.row);
}

void back_from_kind(Stone &stone) {
    if (stone.blank == 0) {
        back_to_template(stone);
        return;
    }
    reopen_word(stone, stone.blank - 1);
}

void back_from_word(Stone &stone) {
    stone.draft.words[stone.blank] = message_no_word;
    enter(stone, Step::Kind, stone.kind);
}

void back_from_confirm(Stone &stone) {
    const std::size_t blanks = blanks_of(stone.draft.template_id);
    if (blanks == 0) {
        back_to_template(stone);
        return;
    }
    reopen_word(stone, blanks - 1);
}

// Back steps back one page; on the first page it closes the menu.
Pressed go_back(Stone &stone) {
    switch (stone.step) {
    case Step::First:
        return {ItemMenuState::Closed, std::nullopt};
    case Step::Mine:
        enter(stone, Step::First, first_mine_index);
        break;
    case Step::Template:
        enter(stone, Step::First, 0);
        break;
    case Step::Kind:
        back_from_kind(stone);
        break;
    case Step::Word:
        back_from_word(stone);
        break;
    case Step::Confirm:
        back_from_confirm(stone);
        break;
    }
    return {};
}

Pressed choose_confirm(Stone &stone) {
    if (stone.cursor == leave_index)
        return {ItemMenuState::Done, stone.draft};
    return go_back(stone);
}

// Confirm chooses. Mine has no rows to choose.
Pressed choose(Stone &stone) {
    switch (stone.step) {
    case Step::First:
        choose_first(stone);
        break;
    case Step::Template:
        choose_template(stone);
        break;
    case Step::Kind:
        choose_kind(stone);
        break;
    case Step::Word:
        choose_word(stone);
        break;
    case Step::Confirm:
        return choose_confirm(stone);
    case Step::Mine:
        break;
    }
    return {};
}

Pressed press_key(Stone &stone, ItemMenuKey key) {
    if (stone.step == Step::Mine && (key == ItemMenuKey::Up || key == ItemMenuKey::Down)) {
        scroll_mine(stone, key);
        return {};
    }
    switch (key) {
    case ItemMenuKey::Up:
        cursor_up(stone);
        return {};
    case ItemMenuKey::Down:
        cursor_down(stone);
        return {};
    case ItemMenuKey::Confirm:
        return choose(stone);
    case ItemMenuKey::Back:
        return go_back(stone);
    }
    return {};
}

} // namespace

MessageWriter::MessageWriter(std::size_t visible_rows) : visible_rows_(std::max<std::size_t>(visible_rows, 1)) {}

MessageWriter::Pressed MessageWriter::press(ItemMenuKey key, std::span<const MessageOut> mine) {
    Stone stone{step_, cursor_, first_visible_, draft_, blank_, kind_, visible_rows_, mine.size()};
    return press_key(stone, key);
}

ItemMenuPage MessageWriter::page(Language language, std::span<const MessageOut> mine) const {
    const std::vector<ItemMenuLine> all = rows_of(step_, kind_, language, mine);
    // Your messages may have shrunk since it was scrolled: the page never starts past its last rows.
    const std::size_t last_first = all.size() > visible_rows_ ? all.size() - visible_rows_ : 0;
    const std::size_t start = std::min(first_visible_, last_first);
    const std::size_t end = std::min(start + visible_rows_, all.size());
    const auto slice = std::span<const ItemMenuLine>(all).subspan(start, end - start);

    ItemMenuPage out;
    out.title = {text_piece(text_in(title_of(step_), language))};
    out.rows.assign(slice.begin(), slice.end());
    out.cursor = step_ == Step::Mine || cursor_ < start ? out.rows.size() : cursor_ - start;
    out.more_above = start > 0;
    out.more_below = end < all.size();
    out.footer = footer_of(step_, draft_, language);
    return out;
}

MessageWriter::Step MessageWriter::step() const {
    return step_;
}

std::size_t MessageWriter::cursor() const {
    return cursor_;
}

std::size_t MessageWriter::first_visible() const {
    return first_visible_;
}

const Message &MessageWriter::draft() const {
    return draft_;
}

std::size_t MessageWriter::blank() const {
    return blank_;
}

} // namespace kf::ext
