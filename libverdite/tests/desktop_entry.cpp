#include <verdite/desktop_entry.h>

#include <doctest/doctest.h>

#include <string>

using namespace verdite;

namespace {
std::string exec_line(std::string_view path) {
    const std::string text = desktop_entry_text({"N", "G", "C", "i", path});
    const auto from = text.find("Exec=");
    return text.substr(from, text.find('\n', from) - from);
}
} // namespace

TEST_CASE("a desktop entry names the program and carries the marker") {
    const std::string text = desktop_entry_text({"Verdite1", "King's Field", "A port.", "verdite1", "/a/b.AppImage"});
    CHECK(text.starts_with("[Desktop Entry]\n"));
    CHECK(text.find("Name=Verdite1\n") != std::string::npos);
    CHECK(text.find("Icon=verdite1\n") != std::string::npos);
    CHECK(text.find(desktop_entry_marker) != std::string::npos);
}

TEST_CASE("Exec survives what a path can hold") {
    CHECK(exec_line("/a/b") == R"(Exec="/a/b")");
    CHECK(exec_line("/my games/v 1") == R"(Exec="/my games/v 1")");
    CHECK(exec_line("/a$b") == R"(Exec="/a\\$b")");
    CHECK(exec_line("/a\"b") == R"(Exec="/a\\"b")");
    CHECK(exec_line("/a`b") == R"(Exec="/a\\`b")");
    CHECK(exec_line("/a\\b") == R"(Exec="/a\\\\b")");
    CHECK(exec_line("/100%") == R"(Exec="/100%%")");
}
