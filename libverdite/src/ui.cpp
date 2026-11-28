// The interface over the game (verdite/ui.h): RmlUi set up with the game's
// files, fonts and host, its documents, input and two canvases.
#include <verdite/ui.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>

#include <verdite/rml_text.h>

#include "ui_canvas_commands.h"
#include "ui_files.h"
#include "ui_recorder.h"

namespace verdite::ui {

namespace {

constexpr float touch_drag_dp = 10;      // a finger moved further is scrolling, not tapping
constexpr const char *canvas_tag = "verdite-canvas";
constexpr const char *canvas_rml =
    "<rml><head><style>"
    "body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }"
    "verdite-canvas { display: block; width: 100%; height: 100%; }"
    "</style></head><body><verdite-canvas/></body></rml>";

// The host for RmlUi's clock, pointer, clipboard and log.
class Ties final : public Rml::SystemInterface {
public:
    explicit Ties(ScreenHost &host) : host_(host) {}
    double GetElapsedTime() override { return host_.seconds(); }
    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
        if (type <= Rml::Log::LT_WARNING) {
            host_.log(message);
        }
        return true;
    }
    void SetMouseCursor(const Rml::String &name) override {
        Cursor cursor = Cursor::Arrow;
        if (name == "pointer") {
            cursor = Cursor::Hand;
        } else if (name == "text") {
            cursor = Cursor::Text;
        }
        host_.set_cursor(cursor);
    }
    void SetClipboardText(const Rml::String &text) override { host_.set_clipboard(text); }
    void GetClipboardText(Rml::String &text) override { text = host_.clipboard(); }

private:
    ScreenHost &host_;
};

Rml::ColourbPremultiplied premultiplied(Colour colour) {
    auto channel = [](float value) {
        return static_cast<Rml::byte>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    float a = std::clamp(colour.a, 0.0f, 1.0f);
    return {channel(colour.r * a), channel(colour.g * a), channel(colour.b * a), channel(a)};
}

// The attribute on an element or its nearest parent that has it, with that element.
std::pair<std::string, Rml::Element *> attribute_up(Rml::Element *element, const char *name) {
    for (; element != nullptr; element = element->GetParentNode()) {
        if (const Rml::Variant *value = element->GetAttribute(name)) {
            return {value->Get<Rml::String>(), element};
        }
    }
    return {{}, nullptr};
}

std::string attribute_of(Rml::Element *element, const char *name) {
    if (element == nullptr) {
        return {};
    }
    const Rml::Variant *value = element->GetAttribute(name);
    return value != nullptr ? value->Get<Rml::String>() : std::string();
}

bool is_text_field(Rml::Element *element) {
    if (element == nullptr) {
        return false;
    }
    if (element->GetTagName() == "textarea") {
        return true;
    }
    if (element->GetTagName() != "input") {
        return false;
    }
    std::string type = attribute_of(element, "type");
    return type.empty() || type == "text" || type == "password";
}

Rml::Input::KeyIdentifier rml_key(TextKey key) {
    switch (key) {
    case TextKey::Backspace: return Rml::Input::KI_BACK;
    case TextKey::Delete: return Rml::Input::KI_DELETE;
    case TextKey::Left: return Rml::Input::KI_LEFT;
    case TextKey::Right: return Rml::Input::KI_RIGHT;
    case TextKey::Up: return Rml::Input::KI_UP;
    case TextKey::Down: return Rml::Input::KI_DOWN;
    case TextKey::Home: return Rml::Input::KI_HOME;
    case TextKey::End: return Rml::Input::KI_END;
    case TextKey::Enter: return Rml::Input::KI_RETURN;
    case TextKey::Tab: return Rml::Input::KI_TAB;
    case TextKey::Escape: return Rml::Input::KI_ESCAPE;
    case TextKey::SelectAll: return Rml::Input::KI_A;
    case TextKey::Copy: return Rml::Input::KI_C;
    case TextKey::Cut: return Rml::Input::KI_X;
    case TextKey::Paste: return Rml::Input::KI_V;
    }
    return Rml::Input::KI_UNKNOWN;
}

int rml_modifiers(TextKey key, KeyMods mods) {
    int out = mods == KeyMods::Shift ? Rml::Input::KM_SHIFT : 0;
    bool with_control = key == TextKey::SelectAll || key == TextKey::Copy || key == TextKey::Cut ||
                        key == TextKey::Paste;
    return with_control ? out | Rml::Input::KM_CTRL : out;
}

// The nearest element from here up that scrolls (content taller than its box).
Rml::Element *scroller_up(Rml::Element *element) {
    for (; element != nullptr; element = element->GetParentNode()) {
        if (element->GetScrollHeight() > element->GetClientHeight() + 1) {
            return element;
        }
    }
    return nullptr;
}

} // namespace

// The fonts, measured for a canvas's text.
class FontMeasure final : public TextMeasure {
public:
    std::string default_family;

    [[nodiscard]] Rml::FontFaceHandle face(std::string_view family, float size_px) const {
        // (RmlUi keeps its families in lower case, as style sheets are read)
        std::string name = Rml::StringUtilities::ToLower(
            std::string(family.empty() ? std::string_view(default_family) : family));
        int size = std::max(static_cast<int>(std::lround(size_px)), 1);
        return Rml::GetFontEngineInterface()->GetFontFaceHandle(
            name, Rml::Style::FontStyle::Normal, Rml::Style::FontWeight::Normal, size);
    }
    Point text_size(std::string_view utf8, float size_px, std::string_view family) const override {
        Rml::FontFaceHandle handle = face(family, size_px);
        if (handle == 0) {
            return {};
        }
        Rml::FontEngineInterface *fonts = Rml::GetFontEngineInterface();
        Rml::String language;
        Rml::TextShapingContext shaping{language};
        int width = fonts->GetStringWidth(handle, Rml::StringView(utf8.data(), utf8.data() + utf8.size()),
                                          shaping);
        return {static_cast<float>(width), fonts->GetFontMetrics(handle).line_spacing};
    }
};

// A canvas drawn as an element filling a document of its own.
class CanvasElement final : public Rml::Element {
public:
    using Rml::Element::Element;
    const FontMeasure *fonts = nullptr;
    std::unique_ptr<Canvas::Commands> commands;

protected:
    void OnRender() override {
        if (!commands || fonts == nullptr) {
            return;
        }
        Rml::RenderManager &render = *GetRenderManager();
        std::vector<Rml::Rectanglei> clips;
        for (CanvasPiece &piece : commands->pieces) {
            switch (piece.kind) {
            case PieceKind::Triangles: render_triangles(render, piece); break;
            case PieceKind::Text: render_text(render, piece); break;
            case PieceKind::PushClip: push_clip(render, clips, piece.clip); break;
            case PieceKind::PopClip: pop_clip(render, clips); break;
            }
        }
        render.DisableScissorRegion();
    }

private:
    static void render_triangles(Rml::RenderManager &render, const CanvasPiece &piece) {
        if (piece.indices.empty()) {
            return;
        }
        Rml::Mesh mesh;
        mesh.vertices.resize(piece.vertices.size());
        for (size_t i = 0; i < piece.vertices.size(); i++) {
            const Vertex &v = piece.vertices[i];
            mesh.vertices[i].position = {v.x, v.y};
            mesh.vertices[i].colour = Rml::ColourbPremultiplied(v.r, v.g, v.b, v.a);
            mesh.vertices[i].tex_coord = {v.u, v.v};
        }
        mesh.indices.assign(piece.indices.begin(), piece.indices.end());
        render.MakeGeometry(std::move(mesh)).Render({0, 0});
    }

    void render_text(Rml::RenderManager &render, const CanvasPiece &piece) const {
        Rml::FontFaceHandle handle = fonts->face(piece.family, piece.size_px);
        if (handle == 0) {
            return;
        }
        Rml::FontEngineInterface *engine = Rml::GetFontEngineInterface();
        Rml::FontEffectsHandle effects = engine->PrepareFontEffects(handle, {});
        float ascent = engine->GetFontMetrics(handle).ascent;
        Rml::String language;
        Rml::TextShapingContext shaping{language};
        Rml::TexturedMeshList meshes;
        engine->GenerateString(render, handle, effects,
                               Rml::StringView(piece.text.data(), piece.text.data() + piece.text.size()),
                               {piece.at.x, piece.at.y + ascent}, premultiplied(piece.colour), 1.0f,
                               shaping, meshes);
        for (Rml::TexturedMesh &mesh : meshes) {
            render.MakeGeometry(std::move(mesh.mesh)).Render({0, 0}, mesh.texture);
        }
    }

    static void push_clip(Rml::RenderManager &render, std::vector<Rml::Rectanglei> &clips, Area area) {
        Rml::Rectanglei region = Rml::Rectanglei::FromPositionSize(
            {static_cast<int>(std::floor(area.x)), static_cast<int>(std::floor(area.y))},
            {std::max(static_cast<int>(std::ceil(area.width)), 0),
             std::max(static_cast<int>(std::ceil(area.height)), 0)});
        if (!clips.empty()) {
            region = region.Intersect(clips.back());
        }
        clips.push_back(region);
        render.SetScissorRegion(region);
    }

    static void pop_clip(Rml::RenderManager &render, std::vector<Rml::Rectanglei> &clips) {
        if (!clips.empty()) {
            clips.pop_back();
        }
        if (clips.empty()) {
            render.DisableScissorRegion();
        } else {
            render.SetScissorRegion(clips.back());
        }
    }
};

struct Document::State {
    Rml::ElementDocument *document = nullptr;
    std::unordered_map<std::string, std::string> given;   // "id|kind": what a part was last given

    Rml::Element *part(std::string_view id) const {
        return document->GetElementById(std::string(id));
    }
    // True when value differs from what the part was last given (and now it was).
    bool changed(std::string_view id, std::string_view kind, std::string_view value) {
        std::string key = std::string(id) + "|" + std::string(kind);
        auto found = given.find(key);
        if (found != given.end() && found->second == value) {
            return false;
        }
        given[key] = std::string(value);
        return true;
    }
};

Document::Document(std::unique_ptr<State> state) : state_(std::move(state)) {}
Document::~Document() = default;

void Document::set_rml(std::string_view id, std::string_view rml) {
    Rml::Element *element = state_->part(id);
    if (element != nullptr && state_->changed(id, "rml", rml)) {
        element->SetInnerRML(std::string(rml));
    }
}

void Document::set_text(std::string_view id, std::string_view text) {
    set_rml(id, rml_escaped(text));
}

void Document::set_classes(std::string_view id, std::string_view classes) {
    Rml::Element *element = state_->part(id);
    if (element != nullptr && state_->changed(id, "class", classes)) {
        element->SetClassNames(std::string(classes));
    }
}

void Document::set_body_classes(std::string_view classes) {
    if (state_->changed("", "body", classes)) {
        state_->document->SetClassNames(std::string(classes));
    }
}

std::string Document::field(std::string_view id) const {
    auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>(state_->part(id));
    return control != nullptr ? control->GetValue() : std::string();
}

void Document::set_field(std::string_view id, std::string_view value) {
    if (auto *control = rmlui_dynamic_cast<Rml::ElementFormControl *>(state_->part(id))) {
        control->SetValue(std::string(value));
    }
}

void Document::focus_field(std::string_view id) {
    if (Rml::Element *element = state_->part(id)) {
        element->Focus(true);
    }
}

void Document::blur() {
    if (Rml::Element *focus = state_->document->GetContext()->GetFocusElement()) {
        if (focus->GetOwnerDocument() == state_->document) {
            focus->Blur();
        }
    }
}

void Document::scroll_to_end(std::string_view id) {
    if (Rml::Element *element = state_->part(id)) {
        element->SetScrollTop(element->GetScrollHeight());
    }
}

void Document::scroll_into_view(std::string_view id) {
    if (Rml::Element *element = state_->part(id)) {
        element->ScrollIntoView(Rml::ScrollIntoViewOptions{Rml::ScrollAlignment::Nearest,
                                                            Rml::ScrollAlignment::Nearest,
                                                            Rml::ScrollBehavior::Instant});
    }
}

void Document::show() {
    if (!state_->document->IsVisible()) {
        state_->document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    }
}

void Document::hide() {
    state_->document->Hide();
}

bool Document::shown() const {
    return state_->document->IsVisible();
}

void Document::raise() {
    state_->document->PullToFront();
}

void Document::lower() {
    state_->document->PushToBack();
}

// Presses, hovers and Enter on the documents, as Actions.
class ActionListener final : public Rml::EventListener {
public:
    std::deque<Action> taken;
    std::unordered_map<Rml::ElementDocument *, std::string> names;

    void ProcessEvent(Rml::Event &event) override {
        Rml::Element *target = event.GetTargetElement();
        std::string document = document_of(target);
        switch (event.GetId()) {
        case Rml::EventId::Mousedown:
            if (event.GetParameter<int>("button", 0) == 0) {
                add(ActionKind::Press, document, target, "data-action");
            }
            break;
        case Rml::EventId::Mouseover:
            add(ActionKind::Hover, document, target, "data-hover");
            break;
        case Rml::EventId::Keydown:
            if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_RETURN) {
                add(ActionKind::Enter, document, target, "data-enter");
            }
            break;
        default:
            break;
        }
    }

private:
    std::string document_of(Rml::Element *element) const {
        if (element == nullptr) {
            return {};
        }
        auto found = names.find(element->GetOwnerDocument());
        return found != names.end() ? found->second : std::string();
    }
    void add(ActionKind kind, const std::string &document, Rml::Element *target, const char *name) {
        auto [value, element] = attribute_up(target, name);
        if (element == nullptr || value.empty()) {
            return;
        }
        taken.push_back({kind, document, std::move(value), attribute_of(element, "data-value")});
    }
};

struct Touch {
    bool down = false;
    std::int64_t finger = 0;
    float start_x = 0, start_y = 0;
    bool scrolling = false;
    Rml::Element *scroller = nullptr;
    float scroll_start = 0;
};

struct Screen::State {
    ScreenHost *host = nullptr;
    Ties ties;
    Files files;
    Recorder recorder;
    FontMeasure fonts;
    bool started = false;
    Rml::Context *context = nullptr;
    std::unique_ptr<Rml::ElementInstancerGeneric<CanvasElement>> canvas_instancer;
    Canvas back{fonts};
    Canvas front{fonts};
    Rml::ElementDocument *back_document = nullptr;
    Rml::ElementDocument *front_document = nullptr;
    CanvasElement *back_element = nullptr;
    CanvasElement *front_element = nullptr;
    ActionListener actions;
    std::map<std::string, std::unique_ptr<Document>, std::less<>> documents;
    int width_px = 1, height_px = 1;
    float px_per_dp = 1;
    Touch touch;

    explicit State(const ScreenSetup &setup)
        : host(setup.host), ties(*setup.host),
          files(setup.assets, [host = setup.host](std::string_view line) { host->log(line); }),
          recorder(setup.images, [host = setup.host](std::string_view line) { host->log(line); }) {}
    ~State() {
        if (started) {
            Rml::Shutdown();   // documents, then the canvas elements' instancer below
        }
    }
    State(const State &) = delete;
    State &operator=(const State &) = delete;

    [[nodiscard]] bool load_fonts(std::span<const FontFace> faces);
    [[nodiscard]] bool open_canvases();
    CanvasElement *canvas_document(Rml::ElementDocument *&document);
};

bool Screen::State::load_fonts(std::span<const FontFace> faces) {
    for (const FontFace &face : faces) {
        std::optional<std::span<const std::byte>> bytes = files.asset(face.asset_path);
        auto weight = face.bold ? Rml::Style::FontWeight::Bold : Rml::Style::FontWeight::Normal;
        if (!bytes ||
            !Rml::LoadFontFace(Rml::Span<const Rml::byte>(
                                   reinterpret_cast<const Rml::byte *>(bytes->data()), bytes->size()),
                               std::string(face.family), Rml::Style::FontStyle::Normal, weight)) {
            host->log("the font " + std::string(face.asset_path) + " did not load");
            return false;
        }
    }
    if (!faces.empty()) {
        fonts.default_family = faces.front().family;
    }
    return true;
}

CanvasElement *Screen::State::canvas_document(Rml::ElementDocument *&document) {
    document = context->LoadDocumentFromMemory(canvas_rml, "[canvas]");
    if (document == nullptr) {
        return nullptr;
    }
    Rml::ElementList found;
    document->GetElementsByTagName(found, canvas_tag);
    auto *element = found.empty() ? nullptr : rmlui_static_cast<CanvasElement *>(found.front());
    if (element != nullptr) {
        element->fonts = &fonts;
        document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    }
    return element;
}

bool Screen::State::open_canvases() {
    back_element = canvas_document(back_document);
    front_element = canvas_document(front_document);
    return back_element != nullptr && front_element != nullptr;
}

Screen::Screen(std::unique_ptr<State> state) : state_(std::move(state)) {}
Screen::~Screen() = default;

std::unique_ptr<Screen> Screen::open(const ScreenSetup &setup) {
    if (setup.host == nullptr) {
        return nullptr;
    }
    auto state = std::make_unique<State>(setup);
    Rml::SetSystemInterface(&state->ties);
    Rml::SetFileInterface(&state->files);
    Rml::SetRenderInterface(&state->recorder);
    if (!Rml::Initialise()) {
        setup.host->log("RmlUi did not start");
        return nullptr;
    }
    state->started = true;
    state->canvas_instancer = std::make_unique<Rml::ElementInstancerGeneric<CanvasElement>>();
    Rml::Factory::RegisterElementInstancer(canvas_tag, state->canvas_instancer.get());
    state->context = Rml::CreateContext("verdite", {state->width_px, state->height_px});
    if (state->context == nullptr || !state->load_fonts(setup.fonts) || !state->open_canvases()) {
        setup.host->log("RmlUi's context, fonts or canvases did not load");
        return nullptr;
    }
    state->context->AddEventListener("mousedown", &state->actions);
    state->context->AddEventListener("mouseover", &state->actions);
    // (in the capture phase: a text field stops its keys going further)
    state->context->AddEventListener("keydown", &state->actions, true);
    return std::unique_ptr<Screen>(new Screen(std::move(state)));
}

void Screen::add_folder(std::string_view prefix, const std::filesystem::path &root) {
    state_->files.add_folder(prefix, root);
}

void Screen::remove_folder(std::string_view prefix) {
    state_->files.remove_folder(prefix);
}

Document *Screen::load(std::string_view path) {
    auto found = state_->documents.find(path);
    if (found != state_->documents.end()) {
        return found->second.get();
    }
    Rml::ElementDocument *document = state_->context->LoadDocument(std::string(path));
    if (document == nullptr) {
        state_->host->log("the document " + std::string(path) + " did not load");
        return nullptr;
    }
    document->Hide();
    state_->actions.names[document] = std::string(path);
    auto doc_state = std::make_unique<Document::State>();
    doc_state->document = document;
    auto made = std::make_unique<Document>(std::move(doc_state));
    Document *borrowed = made.get();
    state_->documents.emplace(std::string(path), std::move(made));
    return borrowed;
}

void Screen::unload(std::string_view path) {
    auto found = state_->documents.find(path);
    if (found == state_->documents.end()) {
        return;
    }
    for (auto it = state_->actions.names.begin(); it != state_->actions.names.end(); ++it) {
        if (it->second == path) {
            state_->context->UnloadDocument(it->first);
            state_->actions.names.erase(it);
            break;
        }
    }
    state_->documents.erase(found);
}

void Screen::resize(int width_px, int height_px, float px_per_dp) {
    state_->width_px = std::max(width_px, 1);
    state_->height_px = std::max(height_px, 1);
    state_->px_per_dp = px_per_dp > 0 ? px_per_dp : 1;
    state_->context->SetDimensions({state_->width_px, state_->height_px});
    state_->context->SetDensityIndependentPixelRatio(state_->px_per_dp);
}

float Screen::px_per_dp() const {
    return state_->px_per_dp;
}

void Screen::pointer_move(float x_px, float y_px) {
    state_->context->ProcessMouseMove(static_cast<int>(x_px), static_cast<int>(y_px), 0);
}

void Screen::pointer_down(float x_px, float y_px) {
    pointer_move(x_px, y_px);
    state_->context->ProcessMouseButtonDown(0, 0);
}

void Screen::pointer_up(float x_px, float y_px) {
    pointer_move(x_px, y_px);
    state_->context->ProcessMouseButtonUp(0, 0);
}

void Screen::pointer_leave() {
    state_->context->ProcessMouseLeave();
}

void Screen::wheel(float steps_down) {
    state_->context->ProcessMouseWheel(Rml::Vector2f(0, steps_down), 0);
}

void Screen::touch_down(std::int64_t finger, float x_px, float y_px) {
    Touch &touch = state_->touch;
    if (touch.down) {
        return;
    }
    touch = Touch{true, finger, x_px, y_px, false, nullptr, 0};
    pointer_move(x_px, y_px);
    touch.scroller = scroller_up(state_->context->GetHoverElement());
    if (touch.scroller != nullptr) {
        touch.scroll_start = touch.scroller->GetScrollTop();
    }
}

void Screen::touch_move(std::int64_t finger, float x_px, float y_px) {
    Touch &touch = state_->touch;
    if (!touch.down || touch.finger != finger) {
        return;
    }
    float moved = std::hypot(x_px - touch.start_x, y_px - touch.start_y);
    if (!touch.scrolling && moved > touch_drag_dp * state_->px_per_dp) {
        touch.scrolling = true;
    }
    if (touch.scrolling && touch.scroller != nullptr) {
        touch.scroller->SetScrollTop(touch.scroll_start - (y_px - touch.start_y));
    }
}

void Screen::touch_up(std::int64_t finger, float x_px, float y_px) {
    Touch &touch = state_->touch;
    if (!touch.down || touch.finger != finger) {
        return;
    }
    bool tap = !touch.scrolling;
    touch = Touch{};
    if (tap) {
        pointer_down(x_px, y_px);
        pointer_up(x_px, y_px);
    }
    pointer_leave();   // (no finger stays hovering)
}

void Screen::key(TextKey key, KeyMods mods) {
    Rml::Input::KeyIdentifier rml = rml_key(key);
    int modifiers = rml_modifiers(key, mods);
    state_->context->ProcessKeyDown(rml, modifiers);
    state_->context->ProcessKeyUp(rml, modifiers);
}

void Screen::text_input(std::string_view utf8) {
    state_->context->ProcessTextInput(std::string(utf8));
}

bool Screen::wants_text() const {
    return is_text_field(state_->context->GetFocusElement());
}

bool Screen::pointer_over_document() const {
    Rml::Element *hover = state_->context->GetHoverElement();
    if (hover == nullptr) {
        return false;
    }
    Rml::ElementDocument *owner = hover->GetOwnerDocument();
    return owner != nullptr && hover != owner && owner != state_->back_document &&
           owner != state_->front_document;
}

std::optional<Action> Screen::take_action() {
    if (state_->actions.taken.empty()) {
        return std::nullopt;
    }
    Action action = std::move(state_->actions.taken.front());
    state_->actions.taken.pop_front();
    return action;
}

Canvas &Screen::canvas(Layer layer) {
    return layer == Layer::Back ? state_->back : state_->front;
}

const Frame &Screen::draw() {
    State &s = *state_;
    s.back_element->commands = s.back.take();
    s.front_element->commands = s.front.take();
    s.back_document->PushToBack();
    s.front_document->PullToFront();
    s.recorder.begin_frame(s.width_px, s.height_px);
    s.context->Update();
    s.context->Render();
    s.recorder.end_frame();
    return s.recorder.frame();
}

} // namespace verdite::ui
