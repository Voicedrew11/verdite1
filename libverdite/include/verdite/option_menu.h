#ifndef VERDITE_OPTION_MENU_H
#define VERDITE_OPTION_MENU_H
// The option table (verdite/options.h) as a menu: its pages are tabs, each
// section a heading and its rows, moved about by a MenuModel
// (verdite/menu_model.h), and written as RML for a document to show. No RmlUi
// here: the RML is text, and the presses come back as the Action names below,
// so a game's own extra tabs (its mods, its console) sit beside these.
//
// The RML, for a game's style sheet:
//   tabs_rml   <tab data-action="tab" data-value="N" class="chosen">Title</tab>
//   rows_rml   <row class="heading">Title</row>, and per row
//              <row id="row-N" data-action="row" data-hover="row" data-value="N"
//                   class="toggle|choice|number|action|status|binding
//                          focused disabled open on">
//                <label>Label</label>
//                <value>On</value> (a Number adds <bar><fill style="width: P%"/></bar>,
//                                   the focused one <arrow data-action="step"
//                                   data-value="N:-1">, and N:1)
//                <fixed>the BindingSlots' fixed RML</fixed>, then
//                <slot data-action="slot" data-value="N:S" class="chosen listening">
//                  the BindingSlots' RML</slot> (a Binding, instead of value)
//                <note>why it is dimmed</note>
//              </row>
//              and under an open Choice: <list><choice data-action="choice"
//              data-value="C" class="highlighted chosen dimmed">Label</choice></list>
//   help_rml   <h2>Label</h2><p>help</p><p class="note">note</p> for the focused row
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <verdite/menu_model.h>
#include <verdite/options.h>

namespace verdite {

// A game's control bindings behind its Binding rows: which rows are bindings
// (a Status row whose id the game knows), their slots, and listening for a new
// control.
class BindingSlots {
public:
    virtual ~BindingSlots() = default;
    // 0: the row is not a binding.
    [[nodiscard]] virtual int slot_count(std::string_view row_id) const = 0;
    // A slot's RML: a pad glyph, a keycap, or text; or what an empty slot shows.
    [[nodiscard]] virtual std::string slot_rml(std::string_view row_id, int slot) const = 0;
    // Controls the row always has, shown before its slots and never chosen
    // (a game's fixed keys); empty for none.
    [[nodiscard]] virtual std::string fixed_rml(std::string_view row_id) const;
    virtual void listen(std::string_view row_id, int slot) = 0;
    virtual void clear(std::string_view row_id, int slot) = 0;
    // Whether that slot is listening for a control now.
    [[nodiscard]] virtual bool listening(std::string_view row_id, int slot) const = 0;
};

class OptionMenu final : public MenuRows {
public:
    // Borrows the table and the bindings (which may be null).
    OptionMenu(OptionTable &table, PlatformFeatures has, BindingSlots *bindings);

    // Reads the table's views again (after any change, and each frame the
    // menu shows: rows come and go with other settings).
    void refresh();

    [[nodiscard]] std::string tabs_rml(const MenuModel &model) const;
    [[nodiscard]] std::string rows_rml(const MenuModel &model) const;
    [[nodiscard]] std::string help_rml(const MenuModel &model) const;
    // The row's id under the menu's focus, or empty.
    [[nodiscard]] std::string focused_id(const MenuModel &model) const;
    // A tab by its page title, or -1.
    [[nodiscard]] int tab_of(std::string_view title) const;

    // A press from the document (tab, row, step, choice, slot, in the RML
    // above) given to the model; false for an action of another name.
    bool press(MenuModel &model, std::string_view action, std::string_view value);
    // The pointer over a row.
    bool hover(MenuModel &model, std::string_view action, std::string_view value);

    int tab_count() const override;
    int row_count(int tab) const override;
    MenuRowShape shape(int tab, int row) const override;
    void choose(int tab, int row, int choice) override;
    void step(int tab, int row, int delta) override;
    void activate(int tab, int row) override;
    void listen(int tab, int row, int slot) override;
    void clear(int tab, int row, int slot) override;

private:
    struct Line {
        std::string heading;               // a heading's title
        const OptionRowView *row = nullptr; // or a row
    };
    [[nodiscard]] const OptionRowView *row_at(int tab, int row) const;
    [[nodiscard]] std::string row_rml(const MenuModel &model, int index, const Line &line) const;
    [[nodiscard]] std::string value_rml(const MenuModel &model, int index,
                                        const OptionRowView &row) const;
    [[nodiscard]] std::string slots_rml(const MenuModel &model, int index,
                                        const OptionRowView &row) const;
    [[nodiscard]] std::string list_rml(const MenuModel &model, const OptionRowView &row) const;

    OptionTable &table_;
    PlatformFeatures has_;
    BindingSlots *bindings_;
    std::vector<OptionPageView> pages_;
    std::vector<std::vector<Line>> lines_;   // per tab
};

// A Choice row's place among its choices for a value, or -1.
[[nodiscard]] int option_choice_index(const OptionRow &row, const OptionValue &value);
// A Number row's value as a share of its range, 0 to 1.
[[nodiscard]] double option_number_share(const OptionRow &row, const OptionValue &value);

} // namespace verdite

#endif // VERDITE_OPTION_MENU_H
