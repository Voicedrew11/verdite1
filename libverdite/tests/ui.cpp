// The interface over the game (verdite/ui.h, verdite/ui_canvas.h): RmlUi run
// headless on carried test files, its frames and Actions read back.
#include <verdite/ui.h>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "../src/ui_files.h"

using namespace verdite::ui;

namespace {

class TestHost final : public ScreenHost {
public:
    double now = 0;
    Cursor cursor = Cursor::Arrow;
    std::string copied;
    std::vector<std::string> lines;

    double seconds() const override { return now; }
    void set_cursor(Cursor shape) override { cursor = shape; }
    void set_clipboard(std::string_view utf8) override { copied = utf8; }
    std::string clipboard() const override { return copied; }
    void log(std::string_view line) override { lines.emplace_back(line); }
};

std::vector<std::byte> read_file(const char *path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
}

std::span<const std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

constexpr std::string_view page_rml = R"(<rml><head><style>
body { font-family: Test; font-size: 16px; color: #fff; }
#go { position: absolute; left: 0; top: 0; width: 100px; height: 100px; }
#name { position: absolute; left: 0; top: 200px; width: 200px; height: 30px; }
#scroll { position: absolute; left: 300px; top: 0; width: 100px; height: 100px; overflow-y: auto; }
#tall { display: block; height: 1000px; }
</style></head><body>
<div id="go" data-action="go" data-hover="over" data-value="7"><span id="label">Go</span></div>
<input id="name" type="text" data-enter="named"/>
<div id="scroll"><div id="tall" data-action="tall">x</div></div>
</body></rml>)";

struct Fixture {
    TestHost host;
    std::vector<std::byte> font = read_file(VERDITE_TEST_FONT);
    std::vector<Asset> assets;
    std::vector<FontFace> fonts;
    std::unique_ptr<Screen> screen;

    Fixture() {
        assets = {{"fonts/test.ttf", font}, {"page.rml", bytes_of(page_rml)}};
        fonts = {{"fonts/test.ttf", "Test"}};
        screen = Screen::open({&host, assets, fonts, nullptr});
        REQUIRE(screen != nullptr);
        screen->resize(640, 480, 1);
    }
    Document &page() {
        Document *document = screen->load("page.rml");
        REQUIRE(document != nullptr);
        document->show();
        screen->draw();   // laid out, so the pointer finds its parts
        return *document;
    }
};

} // namespace

TEST_CASE("ui: a Screen needs a host, and a missing font fails it") {
    CHECK(Screen::open({}) == nullptr);
    TestHost host;
    FontFace missing{"fonts/none.ttf", "None"};
    CHECK(Screen::open({&host, {}, {&missing, 1}, nullptr}) == nullptr);
}

TEST_CASE("ui: a shown document is recorded as draws, its glyphs as uploads") {
    Fixture f;
    const Frame &empty = f.screen->draw();
    CHECK(empty.width_px == 640);
    CHECK(empty.draws.empty());
    Document *loaded = f.screen->load("page.rml");
    REQUIRE(loaded != nullptr);
    Document &page = *loaded;
    page.set_text("label", "Hello <world>");
    page.show();
    const Frame &frame = f.screen->draw();
    CHECK_FALSE(frame.draws.empty());
    for (const Draw &draw : frame.draws) {
        CHECK(draw.first_index + draw.index_count <= frame.indices.size());
    }
    bool glyphs = false;
    for (const TextureUpload &upload : frame.uploads) {
        glyphs |= upload.width_px > 0 && upload.rgba.size() == static_cast<size_t>(upload.width_px) *
                                                                    static_cast<size_t>(upload.height_px) * 4;
    }
    CHECK(glyphs);
    page.hide();
    CHECK(f.screen->draw().draws.empty());
    CHECK_FALSE(page.shown());
}

TEST_CASE("ui: a press and a hover become Actions with the element's value") {
    Fixture f;
    f.page();
    f.screen->pointer_move(10, 10);
    std::optional<Action> hover = f.screen->take_action();
    REQUIRE(hover);
    CHECK(hover->kind == ActionKind::Hover);
    CHECK(hover->name == "over");
    CHECK(hover->value == "7");
    CHECK(hover->document == "page.rml");
    while (f.screen->take_action()) {
    }
    CHECK(f.screen->pointer_over_document());
    f.screen->pointer_down(10, 10);
    std::optional<Action> press = f.screen->take_action();
    REQUIRE(press);
    CHECK(press->kind == ActionKind::Press);
    CHECK(press->name == "go");
    f.screen->pointer_up(10, 10);
    f.screen->pointer_move(600, 400);
    CHECK_FALSE(f.screen->pointer_over_document());
}

TEST_CASE("ui: a tap presses, a drag scrolls and does not") {
    Fixture f;
    f.page();
    f.screen->touch_down(1, 20, 20);
    f.screen->touch_down(2, 20, 20);   // a second finger is ignored
    f.screen->touch_up(1, 20, 20);
    bool pressed = false;
    while (std::optional<Action> action = f.screen->take_action()) {
        pressed |= action->kind == ActionKind::Press && action->name == "go";
    }
    CHECK(pressed);

    f.screen->touch_down(1, 350, 90);
    f.screen->touch_move(1, 350, 20);
    f.screen->touch_up(1, 350, 20);
    while (std::optional<Action> action = f.screen->take_action()) {
        CHECK(action->kind != ActionKind::Press);
    }
}

TEST_CASE("ui: a text field takes typing and Enter") {
    Fixture f;
    Document &page = f.page();
    CHECK_FALSE(f.screen->wants_text());
    page.focus_field("name");
    CHECK(f.screen->wants_text());
    f.screen->text_input("room");
    f.screen->key(TextKey::Backspace, KeyMods::None);
    f.screen->draw();
    CHECK(page.field("name") == "roo");
    f.screen->key(TextKey::Enter, KeyMods::None);
    bool entered = false;
    while (std::optional<Action> action = f.screen->take_action()) {
        entered |= action->kind == ActionKind::Enter && action->name == "named";
    }
    CHECK(entered);
    page.set_field("name", "ABCD");
    CHECK(page.field("name") == "ABCD");
    page.blur();
    CHECK_FALSE(f.screen->wants_text());
}

TEST_CASE("ui: the canvases draw their shapes once, then start empty") {
    Fixture f;
    Canvas &front = f.screen->canvas(Layer::Front);
    front.rect({10, 10, 50, 20}, {1, 0, 0, 1}, Fill::Solid);
    front.circle({100, 100}, 20, {0, 1, 0, 0.5f}, Fill::Outline, 2);
    front.push_clip({0, 0, 50, 50});
    front.text({5, 5}, "Quick saved", {1, 1, 1, 1}, 18, Shadow::Drop);
    const Frame &frame = f.screen->draw();
    REQUIRE(frame.draws.size() >= 3);
    CHECK(frame.draws.front().texture == 0);
    CHECK(frame.draws.front().clipped == false);
    CHECK(frame.draws.back().texture != 0);   // the text's glyphs
    CHECK(frame.draws.back().clipped);
    CHECK(frame.draws.back().clip.width == 50);
    Point size = front.text_size("Quick saved", 18);
    CHECK(size.x > 0);
    CHECK(size.y > 0);
    CHECK(f.screen->draw().draws.empty());
}

TEST_CASE("ui: a canvas wraps text at spaces and line breaks") {
    struct TenPerLetter final : TextMeasure {
        Point text_size(std::string_view text, float, std::string_view) const override {
            return {10.0f * static_cast<float>(text.size()), 12};
        }
    } measure;
    Canvas canvas(measure);
    std::vector<std::string> lines = canvas.wrap("one two three\nfour", 75, 12);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "one two");
    CHECK(lines[1] == "three");
    CHECK(lines[2] == "four");
    CHECK(canvas.wrap("enormousword", 30, 12).size() == 1);
    CHECK(canvas.wrap("", 30, 12).empty());
}

TEST_CASE("ui: a canvas keeps what is drawn on it") {
    struct NoText final : TextMeasure {
        Point text_size(std::string_view, float, std::string_view) const override { return {}; }
    } measure;
    Canvas canvas(measure);
    canvas.rect({0, 0, 10, 10}, {1, 1, 1, 0.5f}, Fill::Solid);
    canvas.polygon(std::vector<Point>{{0, 0}, {1, 1}}, {}, Fill::Solid);   // too few points
    canvas.pop_clip();   // nothing to pop
    CHECK_FALSE(canvas.empty());
}

TEST_CASE("ui: a folder's files never come from outside it") {
    std::filesystem::path root = std::filesystem::temp_directory_path() / "verdite_ui_folder";
    std::filesystem::create_directories(root / "docs");
    std::ofstream(root / "docs" / "panel.rml") << "<rml><body><div id=\"x\">mod</div></body></rml>";
    CHECK(folder_file(root, "docs/panel.rml").has_value());
    CHECK_FALSE(folder_file(root, "../outside.rml").has_value());
    CHECK_FALSE(folder_file(root, "docs/../../outside.rml").has_value());
    CHECK_FALSE(folder_file(root, "/etc/passwd").has_value());
    CHECK_FALSE(folder_file(root, "").has_value());
    CHECK(asset_name("./menu.rml") == "menu.rml");
    CHECK(asset_name("/fonts/a.ttf") == "fonts/a.ttf");

    Fixture f;
    f.screen->add_folder("mods/test", root);
    Document *panel = f.screen->load("mods/test/docs/panel.rml");
    CHECK(panel != nullptr);
    CHECK(f.screen->load("mods/test/../page.rml") == nullptr);
    f.screen->unload("mods/test/docs/panel.rml");
    f.screen->remove_folder("mods/test");
    CHECK(f.screen->load("mods/test/docs/panel.rml") == nullptr);
    std::filesystem::remove_all(root);
}

namespace {
std::vector<std::byte> mask_file(std::uint32_t width_px, std::uint32_t height_px, size_t alpha_bytes) {
    std::vector<std::byte> file = {std::byte{'K'}, std::byte{'F'}, std::byte{'4'}, std::byte{'A'}};
    for (std::uint32_t side : {width_px, height_px}) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            file.push_back(static_cast<std::byte>((side >> shift) & 0xFFU));
        }
    }
    for (size_t i = 0; i < alpha_bytes; i++) {
        file.push_back(static_cast<std::byte>(i * 40));
    }
    return file;
}
} // namespace

TEST_CASE("ui: an alpha mask becomes a white premultiplied texture; a broken one is nothing") {
    const std::optional<TextureUpload> mask = alpha_mask(mask_file(3, 2, 6));
    REQUIRE(mask.has_value());
    CHECK(mask->width_px == 3);
    CHECK(mask->height_px == 2);
    REQUIRE(mask->rgba.size() == 24);
    CHECK(mask->rgba[5 * 4] == 200);
    CHECK(mask->rgba[5 * 4 + 3] == 200);

    CHECK_FALSE(alpha_mask(mask_file(3, 2, 5)).has_value());    // short
    CHECK_FALSE(alpha_mask(mask_file(3, 2, 7)).has_value());    // long
    CHECK_FALSE(alpha_mask(mask_file(0, 2, 0)).has_value());    // empty
    CHECK_FALSE(alpha_mask(mask_file(9000, 1, 0)).has_value()); // too wide
    CHECK_FALSE(alpha_mask(mask_file(0x40000000, 4, 0)).has_value());   // (size_t overflow on 32-bit)
    std::vector<std::byte> wrong = mask_file(1, 1, 1);
    wrong[3] = std::byte{'B'};
    CHECK_FALSE(alpha_mask(wrong).has_value());
    CHECK_FALSE(alpha_mask({}).has_value());
}

TEST_CASE("ui: a document's img draws a carried alpha mask") {
    TestHost host;
    std::vector<std::byte> font = read_file(VERDITE_TEST_FONT);
    const std::vector<std::byte> mask = mask_file(2, 2, 4);
    constexpr std::string_view rml = R"(<rml><body><img src="logo.alpha"/></body></rml>)";
    const std::vector<Asset> assets = {{"fonts/test.ttf", font}, {"logo.alpha", mask}, {"img.rml", bytes_of(rml)}};
    const std::vector<FontFace> fonts = {{"fonts/test.ttf", "Test"}};
    std::unique_ptr<Screen> screen = Screen::open({&host, assets, fonts, nullptr});
    REQUIRE(screen != nullptr);
    screen->resize(64, 64, 1);
    Document *page = screen->load("img.rml");
    REQUIRE(page != nullptr);
    page->show();
    const Frame &frame = screen->draw();
    bool drawn = false;
    for (const TextureUpload &upload : frame.uploads) {
        drawn |= upload.width_px == 2 && upload.height_px == 2;
    }
    CHECK(drawn);
}
