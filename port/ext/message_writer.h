#pragma once
// The message stone's menu, with no drawing and no network (docs/MESSAGES_PLAN.md, "Writing"):
// a first page of Leave a message and Your messages; leaving one picks a template, then for each
// blank a kind of word and the word, then shows the message to leave it or step back. Back steps
// back a page, and closes the menu from the first.
#include "messages.h"

#include <kf/platform/extra_items.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace kf::ext {

// One of the player's own messages out, for Your messages.
struct MessageOut {
    Message message;
    std::uint32_t good = 0;
};

class MessageWriter {
public:
    enum class Step : std::uint8_t { First, Mine, Template, Kind, Word, Confirm };
    struct Pressed {
        ItemMenuState state = ItemMenuState::Open;
        std::optional<Message> left;   // set with Done: the message to post here
    };

    // visible_rows: how many rows a page shows at once; a longer list scrolls.
    explicit MessageWriter(std::size_t visible_rows);

    // mine is Your messages' list, as page has it: Up and Down scroll it.
    [[nodiscard]] Pressed press(ItemMenuKey key, std::span<const MessageOut> mine);
    // The page now, in language; mine fills Your messages.
    [[nodiscard]] ItemMenuPage page(Language language, std::span<const MessageOut> mine) const;

    [[nodiscard]] Step step() const;
    [[nodiscard]] std::size_t cursor() const;          // into the step's whole list
    [[nodiscard]] std::size_t first_visible() const;
    [[nodiscard]] const Message &draft() const;        // words are message_no_word until chosen
    [[nodiscard]] std::size_t blank() const;           // the blank Kind and Word fill

private:
    std::size_t visible_rows_;
    Step step_ = Step::First;
    std::size_t cursor_ = 0, first_visible_ = 0;
    Message draft_;
    std::size_t blank_ = 0;
    std::size_t kind_ = 0;                             // the chosen kind, for Word
};

} // namespace kf::ext
