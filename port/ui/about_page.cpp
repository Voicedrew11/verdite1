// The F1 menu's About page: the build and whose work it stands on.
#include "menu_page.h"

#include "build_info.h"

#include <kf/platform/user_folder.h>
#include <verdite/rml_text.h>
#include <verdite/ui.h>

namespace kf::ext {
namespace {
class AboutPage final : public MenuPage {
public:
    std::string title() const override { return "About"; }

    std::string part_rml() const override {
        return "<h2>" + verdite::rml_escaped(std::string(product_name) + " " + KF_VERSION) + "</h2>" +
               "<p class=\"dim\">" + verdite::rml_escaped(KF_BUILD_COMMIT) + "</p>"
               "<div class=\"page-text\">"
               "<p>King's Field (SLPS-00017) source port, with PC extensions.</p>"
               "<p>Source port: sushi-shi/kings-field-decomp (CC0).</p>"
               "<p>Overlay, settings and mod workflow inspired by Verdite2 (Voicedrew11, MIT).</p>"
               "<p>English translation: John Osborne, Kaz; save fix: HwitVlf.</p>"
               "<p>RmlUi (MIT), FreeType (FreeType License), Lua 5.4 (MIT), cJSON (MIT), "
               "SDL3 (zlib).</p>"
               "</div>";
    }
    void update(verdite::ui::Document &) override {}
};
} // namespace

std::unique_ptr<MenuPage> make_about_page() {
    return std::make_unique<AboutPage>();
}

} // namespace kf::ext
