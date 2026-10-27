#include <verdite/title_menu.h>

#include <algorithm>
#include <utility>

namespace verdite {
namespace {

constexpr std::size_t public_letter_bits = 5;   // public_code_letters has 32
constexpr std::size_t join_grid_columns = 5;

bool is_digit(char c) {
    return private_code_digits.find(c) != std::string_view::npos;
}

bool is_public_letter(char c) {
    return public_code_letters.find(c) != std::string_view::npos;
}

// splitmix64: a new room's code from the menu's seed.
std::uint64_t next_random(std::uint64_t &state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

TitleRowView row(TitleRowKind kind, bool enabled = true) {
    TitleRowView view;
    view.kind = kind;
    view.enabled = enabled;
    return view;
}

// The page Back goes to, and the row on it that leads here.
struct Parent {
    TitlePage page;
    TitleRowKind row;
};

std::optional<Parent> parent_of(TitlePage page) {
    switch (page) {
    case TitlePage::Continue: return Parent{TitlePage::Top, TitleRowKind::Continue};
    case TitlePage::Coop: return Parent{TitlePage::Top, TitleRowKind::Coop};
    case TitlePage::Host: return Parent{TitlePage::Coop, TitleRowKind::Host};
    case TitlePage::HostCode: return Parent{TitlePage::Host, TitleRowKind::NewPrivateWorld};
    case TitlePage::JoinCode: return Parent{TitlePage::Coop, TitleRowKind::JoinCode};
    case TitlePage::RoomList: return Parent{TitlePage::Coop, TitleRowKind::RoomList};
    case TitlePage::Top:
    case TitlePage::Waiting: return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::string room_code_private(std::uint64_t random_bits) {
    std::string code;
    for (std::size_t i = 0; i < private_code_length; ++i) {
        code += private_code_digits[random_bits % private_code_digits.size()];
        random_bits /= private_code_digits.size();
    }
    return code;
}

std::string room_code_public(std::uint64_t random_bits) {
    std::string code = "P";
    for (std::size_t i = 1; i < public_code_length; ++i) {
        code += public_code_letters[random_bits & ((1u << public_letter_bits) - 1)];
        random_bits >>= public_letter_bits;
    }
    return code;
}

bool room_code_valid(std::string_view code) {
    return room_code_is_public(code) ||
           (code.size() == private_code_length && std::ranges::all_of(code, is_digit));
}

bool room_code_is_public(std::string_view code) {
    return code.size() == public_code_length && code.front() == 'P' && std::ranges::all_of(code, is_public_letter);
}

TitleMenu::TitleMenu(std::uint64_t random_seed) : random_(random_seed) {}

void TitleMenu::open(std::vector<TitleSave> saves, std::vector<TitleWorld> worlds) {
    saves_ = std::move(saves);
    worlds_ = std::move(worlds);
    rooms_.clear();
    list_state_ = RoomListState::Connecting;
    code_.clear();
    failure_.clear();
    choice_.reset();
    const bool any_save = std::ranges::any_of(saves_, &TitleSave::used);
    go(TitlePage::Top, any_save ? TitleRowKind::Continue : TitleRowKind::NewGame);
}

void TitleMenu::go(TitlePage page, std::optional<TitleRowKind> on) {
    page_ = page;
    row_ = 0;
    if (!on)
        return;
    const auto rows = view().rows;
    const auto found = std::ranges::find(rows, *on, &TitleRowView::kind);
    if (found != rows.end())
        row_ = static_cast<std::size_t>(found - rows.begin());
}

void TitleMenu::press(TitleInput input) {
    failure_.clear();
    const std::size_t count = view().rows.size();
    switch (input) {
    case TitleInput::Up: move_vertically(-1, count); break;
    case TitleInput::Down: move_vertically(1, count); break;
    case TitleInput::Left: move_in_grid(-1); break;
    case TitleInput::Right: move_in_grid(1); break;
    case TitleInput::Confirm: confirm(); break;
    case TitleInput::Back: back(); break;
    }
}

void TitleMenu::move_vertically(int delta, std::size_t count) {
    if (count == 0)
        return;
    const std::size_t letters = private_code_digits.size();
    if (page_ == TitlePage::JoinCode && row_ < letters) {
        // Down a column of the grid to DELETE below its last line; up from its first to JOIN.
        if (delta > 0)
            row_ = row_ + join_grid_columns < letters ? row_ + join_grid_columns : letters;
        else
            row_ = row_ >= join_grid_columns ? row_ - join_grid_columns : count - 1;
        return;
    }
    if (page_ == TitlePage::JoinCode && row_ == letters && delta < 0) {
        row_ = letters - join_grid_columns;
        return;
    }
    const auto signed_count = static_cast<std::ptrdiff_t>(count);
    row_ = static_cast<std::size_t>(((static_cast<std::ptrdiff_t>(row_) + delta) % signed_count + signed_count) % signed_count);
}

void TitleMenu::move_in_grid(int delta) {
    if (page_ != TitlePage::JoinCode || row_ >= private_code_digits.size())
        return;
    const std::size_t line = row_ / join_grid_columns * join_grid_columns;
    const auto columns = static_cast<std::ptrdiff_t>(join_grid_columns);
    const auto column = static_cast<std::ptrdiff_t>(row_ - line);
    row_ = line + static_cast<std::size_t>(((column + delta) % columns + columns) % columns);
}

void TitleMenu::confirm() {
    const auto rows = view().rows;
    if (row_ >= rows.size() || !rows[row_].enabled)
        return;
    const TitleRowView &chosen = rows[row_];
    switch (chosen.kind) {
    case TitleRowKind::Continue: go(TitlePage::Continue, std::nullopt); break;
    case TitleRowKind::NewGame: choice_ = TitleChoice{TitleAction::NewGame, 0, {}, false}; break;
    case TitleRowKind::Coop: go(TitlePage::Coop, std::nullopt); break;
    case TitleRowKind::Slot: choice_ = TitleChoice{TitleAction::Continue, row_, {}, false}; break;
    case TitleRowKind::Host: go(TitlePage::Host, std::nullopt); break;
    case TitleRowKind::JoinCode:
        code_.clear();
        go(TitlePage::JoinCode, std::nullopt);
        break;
    case TitleRowKind::RoomList:
        list_state_ = RoomListState::Connecting;
        rooms_.clear();
        go(TitlePage::RoomList, std::nullopt);
        break;
    case TitleRowKind::NewPrivateWorld:
        code_ = room_code_private(next_random(random_));
        go(TitlePage::HostCode, TitleRowKind::Start);
        break;
    case TitleRowKind::NewPublicWorld: wait_for({TitleAction::Host, 0, room_code_public(next_random(random_)), true}); break;
    case TitleRowKind::KeptWorld:
        wait_for({TitleAction::Host, 0, chosen.world->code, room_code_is_public(chosen.world->code)});
        break;
    case TitleRowKind::Start: wait_for({TitleAction::Host, 0, code_, false}); break;
    case TitleRowKind::Letter: type(chosen.letter); break;
    case TitleRowKind::Delete: erase(); break;
    case TitleRowKind::Join: wait_for({TitleAction::Join, 0, code_, false}); break;
    case TitleRowKind::Room: wait_for({TitleAction::Join, 0, chosen.room->code, false}); break;
    }
}

void TitleMenu::wait_for(TitleChoice choice) {
    waiting_from_ = page_;
    waiting_row_ = row_;
    code_ = choice.code;
    choice_ = std::move(choice);
    page_ = TitlePage::Waiting;
    row_ = 0;
}

void TitleMenu::back() {
    if (page_ == TitlePage::Waiting) {
        choice_ = TitleChoice{TitleAction::CancelJoin, 0, code_, false};
        return_from_waiting();
        return;
    }
    if (const auto parent = parent_of(page_))
        go(parent->page, parent->row);
}

void TitleMenu::return_from_waiting() {
    page_ = waiting_from_;
    row_ = waiting_row_;
    if (page_ == TitlePage::HostCode || page_ == TitlePage::JoinCode)
        return;   // keeps the code
    code_.clear();
}

void TitleMenu::type(char digit) {
    if (page_ != TitlePage::JoinCode)
        return;
    failure_.clear();
    if (!is_digit(digit) || code_.size() >= private_code_length)
        return;
    code_ += digit;
}

void TitleMenu::erase() {
    if (page_ != TitlePage::JoinCode)
        return;
    failure_.clear();
    if (!code_.empty())
        code_.pop_back();
}

void TitleMenu::point(std::size_t row) {
    if (row < view().rows.size())
        row_ = row;
}

void TitleMenu::set_rooms(std::vector<TitleRoom> rooms, RoomListState state) {
    std::string on;
    if (page_ == TitlePage::RoomList && row_ < rooms_.size())
        on = rooms_[row_].code;
    rooms_ = std::move(rooms);
    list_state_ = state;
    if (page_ != TitlePage::RoomList)
        return;
    const auto found = std::ranges::find(rooms_, on, &TitleRoom::code);
    if (found != rooms_.end())
        row_ = static_cast<std::size_t>(found - rooms_.begin());
    else
        row_ = std::min(row_, rooms_.empty() ? std::size_t{0} : rooms_.size() - 1);
}

bool TitleMenu::wants_room_list() const {
    return page_ == TitlePage::RoomList;
}

std::optional<TitleChoice> TitleMenu::take_choice() {
    return std::exchange(choice_, std::nullopt);
}

void TitleMenu::coop_started() {
    page_ = TitlePage::Top;
    row_ = 0;
}

void TitleMenu::coop_failed(std::string why) {
    if (page_ != TitlePage::Waiting)
        return;
    return_from_waiting();
    failure_ = std::move(why);
}

TitleView TitleMenu::view() const {
    TitleView view;
    view.page = page_;
    view.code = code_;
    view.list_state = list_state_;
    view.failure = failure_;
    auto &rows = view.rows;
    switch (page_) {
    case TitlePage::Top:
        rows.push_back(row(TitleRowKind::Continue, std::ranges::any_of(saves_, &TitleSave::used)));
        rows.push_back(row(TitleRowKind::NewGame));
        rows.push_back(row(TitleRowKind::Coop));
        break;
    case TitlePage::Continue:
        for (const TitleSave &save : saves_) {
            rows.push_back(row(TitleRowKind::Slot, save.used));
            rows.back().save = &save;
        }
        break;
    case TitlePage::Coop:
        rows.push_back(row(TitleRowKind::Host));
        rows.push_back(row(TitleRowKind::JoinCode));
        rows.push_back(row(TitleRowKind::RoomList));
        break;
    case TitlePage::Host:
        rows.push_back(row(TitleRowKind::NewPrivateWorld));
        rows.push_back(row(TitleRowKind::NewPublicWorld));
        for (const TitleWorld &world : worlds_) {
            rows.push_back(row(TitleRowKind::KeptWorld));
            rows.back().world = &world;
        }
        break;
    case TitlePage::HostCode: rows.push_back(row(TitleRowKind::Start)); break;
    case TitlePage::JoinCode:
        view.grid_columns = join_grid_columns;
        for (const char letter : private_code_digits) {
            rows.push_back(row(TitleRowKind::Letter));
            rows.back().letter = letter;
        }
        rows.push_back(row(TitleRowKind::Delete, !code_.empty()));
        rows.push_back(row(TitleRowKind::Join, code_.size() == private_code_length));
        break;
    case TitlePage::RoomList:
        for (const TitleRoom &room : rooms_) {
            rows.push_back(row(TitleRowKind::Room, room.players < room.most));
            rows.back().room = &room;
        }
        break;
    case TitlePage::Waiting: break;
    }
    if (row_ < rows.size())
        rows[row_].selected = true;
    return view;
}

} // namespace verdite
