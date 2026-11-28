# The interface's files (documents, style sheets, fonts, masks) as C++ arrays, so the game carries them inside it:
# the table verdite::ui::Asset reads (verdite/ui.h). Run as a script by the build
# (cmake -DOUT=... -DNAME=kf1_ui_assets -DENTRIES="root|a^root|b" -P cmake/ui_assets.cmake), again whenever one
# changes. Each file is named by its path under its root, as the documents name it; libverdite's shared files and a
# game's own join in one table.
string(REPLACE "^" ";" ENTRIES "${ENTRIES}")
set(body "")
set(table "")
set(index 0)
foreach(entry ${ENTRIES})
  string(FIND "${entry}" "|" bar)
  string(SUBSTRING "${entry}" 0 ${bar} root)
  math(EXPR after "${bar} + 1")
  string(SUBSTRING "${entry}" ${after} -1 name)
  file(READ "${root}/${name}" hex HEX)
  string(LENGTH "${hex}" digits)
  math(EXPR size "${digits} / 2")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
  string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" bytes "${bytes}")
  string(APPEND body "const unsigned char asset_${index}[] = {\n${bytes}0};\n")
  string(APPEND table "    {\"${name}\", {reinterpret_cast<const std::byte *>(asset_${index}), ${size}}},\n")
  math(EXPR index "${index} + 1")
endforeach()
file(WRITE "${OUT}.new"
  "// Made by cmake/ui_assets.cmake: do not edit\n"
  "#include <verdite/ui.h>\n"
  "namespace {\n${body}\n"
  "const verdite::ui::Asset assets[] = {\n${table}};\n"
  "} // namespace\n"
  "std::span<const verdite::ui::Asset> ${NAME}() { return assets; }\n")
file(COPY_FILE "${OUT}.new" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.new")
