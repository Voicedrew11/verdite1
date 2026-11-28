#ifndef VERDITE_RML_TEXT_H
#define VERDITE_RML_TEXT_H
// Text made safe to put inside RML (the interface's documents, verdite/ui.h).
#include <string>
#include <string_view>

namespace verdite {

// & < > " ' as entities.
[[nodiscard]] std::string rml_escaped(std::string_view text);

} // namespace verdite

#endif // VERDITE_RML_TEXT_H
