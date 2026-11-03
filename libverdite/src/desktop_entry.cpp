#include <verdite/desktop_entry.h>

namespace verdite {
namespace {
constexpr std::string_view exec_escaped = "\"`$\\";

// The Exec key's quoting, then the string type's: the spec has both apply, so a backslash inside the
// quotes is written four times. A % is doubled, or it reads as a field code.
std::string exec_quoted(std::string_view path) {
    std::string out = "\"";
    for (const char c : path) {
        if (c == '%') {
            out += "%%";
            continue;
        }
        if (exec_escaped.find(c) != std::string_view::npos)
            out += c == '\\' ? "\\\\\\" : "\\\\";
        out += c;
    }
    return out + "\"";
}
} // namespace

std::string desktop_entry_text(const DesktopEntryText &entry) {
    std::string text = "[Desktop Entry]\nType=Application\n";
    text += "Name=" + std::string(entry.name) + "\n";
    text += "GenericName=" + std::string(entry.generic_name) + "\n";
    text += "Comment=" + std::string(entry.comment) + "\n";
    text += "Exec=" + exec_quoted(entry.executable) + "\n";
    text += "Icon=" + std::string(entry.icon) + "\n";
    text += "Terminal=false\nCategories=Game;RolePlaying;\n";
    text += std::string(desktop_entry_marker) + "\n";
    return text;
}

} // namespace verdite
