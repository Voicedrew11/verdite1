package io.github.voicedrew11.verdite1;

import android.app.Activity;
import android.content.Context;
import android.util.Log;
import android.util.TypedValue;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import androidx.appcompat.view.ContextThemeWrapper;
import androidx.appcompat.widget.PopupMenu;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.chip.Chip;
import com.google.android.material.chip.ChipGroup;
import com.google.android.material.color.DynamicColors;
import com.google.android.material.color.MaterialColors;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.slider.Slider;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.IllegalFormatException;
import java.util.List;
import java.util.Locale;
import java.util.Map;

// The first-run setup as Material 3 views, full screen, with the game suspended under it: a fixed
// head and foot around a scrolling page, laid out for a landscape phone's short screen. The page
// comes from the game as JSON (verdite::setup_json); a tap goes back as a Setup::command line, the
// game answers with the new view, and the rows update in place so the focus and the scroll stay.
// On the UI thread throughout.
final class SetupPanel {
    private static final String TAG = "Verdite1";
    private static final int MAX_CONTENT_DP = 960;      // a tablet keeps the page readable
    private static final int WIDE_DP = 560;             // from here, a label beside its control
    private static final int MOST_CHIPS = 4;            // more choices than this open a list
    private static final String KEEP = "Keep as is";
    private static final String KEEP_HELP = "Set by hand. Choose one of these to replace it, or keep it as it is.";

    static native String nativeSetupJson();
    static native void nativeSetupControl(String line);

    // One row of a page, built once and shown again with each new view.
    private interface Row {
        View view();
        void show(JSONObject item) throws JSONException;
    }

    private final Activity activity;
    private final ViewGroup host;           // SDL's layout, over its surface
    private final Runnable giveBackFocus;   // to SDL's surface, so keys and pads reach the game
    private final Context ui;               // the Material theme
    private FrameLayout root;
    private LinearLayout content, tierChips;
    private TextView title, step;
    private ScrollView scroll;
    private MaterialButton backButton, forwardButton;
    private String builtPage = "";
    private String shownStep = "";
    private final Map<String, Row> rows = new HashMap<>();
    private String dragging;                // a slider under a finger is not moved by updates
    private boolean open;

    SetupPanel(Activity activity, ViewGroup host, Runnable giveBackFocus) {
        this.activity = activity;
        this.host = host;
        this.giveBackFocus = giveBackFocus;
        this.ui = DynamicColors.wrapContextIfAvailable(new ContextThemeWrapper(activity, R.style.Theme_Verdite_Settings));
    }

    boolean isOpen() {
        return open;
    }

    // Every new view from the game, the first one opening the panel.
    void show() {
        if (root == null)
            build();
        if (root.getParent() == null)
            host.addView(root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        if (!open) {
            open = true;
            root.setVisibility(View.VISIBLE);
            root.bringToFront();
            root.animate().cancel();
            root.setAlpha(0f);
            root.animate().alpha(1f).setDuration(150).start();
            root.requestApplyInsets();
        }
        refresh();
    }

    void close() {
        if (!open)
            return;
        open = false;
        dragging = null;
        builtPage = "";
        root.animate().alpha(0f).setDuration(120).withEndAction(() -> {
            if (!open)
                root.setVisibility(View.GONE);
        }).start();
        giveBackFocus.run();
    }

    // Android's Back and a pad's B: a step back, never off the welcome, where only its button skips.
    void back() {
        if (!shownStep.equals("welcome"))
            nativeSetupControl("back");
    }

    // A pad's Start: the way on.
    void forward() {
        if (shownStep.equals("page"))
            nativeSetupControl("next");
        else if (shownStep.equals("summary"))
            nativeSetupControl("finish");
    }

    private void refresh() {
        try {
            JSONObject view = new JSONObject(nativeSetupJson());
            shownStep = view.getString("step");
            showHead(view);
            showFoot(view);
            String key = pageKey(view);
            boolean newPage = !key.equals(builtPage);
            if (newPage)
                buildPage(view, key);
            showRows(view);
            if (newPage)
                focusFirst(view);
        } catch (JSONException e) {
            Log.w(TAG, "setup: " + e);
        }
    }

    // ---- the frame ----

    private void build() {
        root = new FrameLayout(activity);
        root.setClickable(true);   // touches stay on the panel, off SDL's surface
        root.setBackgroundColor(color(com.google.android.material.R.attr.colorSurface));
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            fitCutout(insets);
            return insets;
        });

        LinearLayout column = new LinearLayout(ui);
        column.setOrientation(LinearLayout.VERTICAL);
        FrameLayout.LayoutParams centred = new FrameLayout.LayoutParams(
                Math.min(dp(MAX_CONTENT_DP), activity.getResources().getDisplayMetrics().widthPixels),
                ViewGroup.LayoutParams.MATCH_PARENT);
        centred.gravity = Gravity.CENTER_HORIZONTAL;
        root.addView(column, centred);

        LinearLayout head = new LinearLayout(ui);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.setPadding(dp(24), dp(12), dp(24), dp(8));
        title = text("", com.google.android.material.R.style.TextAppearance_Material3_TitleLarge,
                com.google.android.material.R.attr.colorOnSurface);
        head.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        step = text("", com.google.android.material.R.style.TextAppearance_Material3_LabelLarge,
                com.google.android.material.R.attr.colorOnSurfaceVariant);
        head.addView(step);
        column.addView(head);

        scroll = new ScrollView(ui);
        content = new LinearLayout(ui);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), 0, dp(24), dp(12));
        scroll.addView(content);
        column.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        LinearLayout foot = new LinearLayout(ui);
        foot.setOrientation(LinearLayout.HORIZONTAL);
        foot.setGravity(Gravity.CENTER_VERTICAL | Gravity.END);
        foot.setPadding(dp(24), dp(4), dp(24), dp(8));
        backButton = new MaterialButton(ui, null, com.google.android.material.R.attr.materialButtonOutlinedStyle);
        backButton.setAllCaps(false);
        foot.addView(backButton);
        forwardButton = new MaterialButton(ui);
        forwardButton.setAllCaps(false);
        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        gap.setMarginStart(dp(12));
        foot.addView(forwardButton, gap);
        column.addView(foot);
    }

    // A cutout, on either side in landscape, would cover the page's edge.
    private void fitCutout(WindowInsets insets) {
        DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout == null)
            return;
        root.setPadding(cutout.getSafeInsetLeft(), cutout.getSafeInsetTop(), cutout.getSafeInsetRight(), cutout.getSafeInsetBottom());
    }

    private void showHead(JSONObject view) throws JSONException {
        switch (shownStep) {
        case "welcome":
            title.setText(view.getJSONObject("welcome").getString("title"));
            step.setText("");
            break;
        case "summary":
            title.setText("Ready");
            step.setText(stepText(view));
            break;
        default:
            title.setText(view.getString("title"));
            step.setText(stepText(view));
            break;
        }
    }

    private static String stepText(JSONObject view) throws JSONException {
        return "Step " + (view.getInt("page") + 1) + " of " + (view.getInt("pageCount") + 1);
    }

    private void showFoot(JSONObject view) throws JSONException {
        if (shownStep.equals("welcome")) {
            backButton.setText(view.getJSONObject("welcome").getString("skipLabel"));
            backButton.setOnClickListener(v -> nativeSetupControl("skip"));
            forwardButton.setVisibility(View.GONE);
            return;
        }
        backButton.setText("Back");
        backButton.setOnClickListener(v -> nativeSetupControl("back"));
        forwardButton.setVisibility(View.VISIBLE);
        boolean last = shownStep.equals("summary");
        forwardButton.setText(last ? "Finish" : "Next");
        forwardButton.setOnClickListener(v -> nativeSetupControl(last ? "finish" : "next"));
    }

    // What decides that the page must be built anew: another step, or rows that came or went.
    private static String pageKey(JSONObject view) throws JSONException {
        StringBuilder key = new StringBuilder(view.getString("step")).append(view.getInt("page"));
        JSONArray items = view.getJSONArray("items");
        for (int i = 0; i < items.length(); i++)
            key.append('\n').append(itemId(items.getJSONObject(i)));
        return key.toString();
    }

    private static String itemId(JSONObject item) throws JSONException {
        return item.getString("type").equals("group") ? item.getString("id") : item.getJSONObject("row").getString("id");
    }

    private void buildPage(JSONObject view, String key) throws JSONException {
        boolean samePage = builtPage.startsWith(view.getString("step") + view.getInt("page") + "\n");
        int at = scroll.getScrollY();
        content.removeAllViews();
        rows.clear();
        tierChips = null;
        switch (shownStep) {
        case "welcome":
            buildWelcome(view);
            break;
        case "summary":
            buildSummary(view);
            break;
        default:
            buildRows(view);
            break;
        }
        builtPage = key;
        int to = samePage ? at : 0;
        scroll.post(() -> scroll.scrollTo(0, to));
    }

    private void showRows(JSONObject view) throws JSONException {
        JSONArray items = view.getJSONArray("items");
        for (int i = 0; i < items.length(); i++) {
            JSONObject item = items.getJSONObject(i);
            Row built = rows.get(itemId(item));
            if (built != null)
                built.show(item);
        }
        if (tierChips != null)
            showTiers(view);
    }

    // The first thing a pad's D-pad lands on: Modern on the welcome, Next on a page, Finish on the summary.
    private void focusFirst(JSONObject view) {
        View target = forwardButton;
        if (shownStep.equals("welcome"))
            target = content.findViewWithTag("first");
        else if (shownStep.equals("page") && content.getChildCount() > 0)
            target = content;
        View focus = target == null ? forwardButton : target;
        focus.post(focus::requestFocus);
    }

    // ---- the welcome ----

    private void buildWelcome(JSONObject view) throws JSONException {
        JSONObject words = view.getJSONObject("welcome");
        content.addView(body(words.getString("text")));
        TextView prompt = dim(words.getString("prompt"));
        prompt.setPadding(0, dp(4), 0, dp(12));
        content.addView(prompt);

        JSONArray presets = view.getJSONArray("presets");
        LinearLayout cards = new LinearLayout(ui);
        cards.setOrientation(wide() ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        content.addView(cards);
        int lastTiered = -1;
        for (int i = 0; i < presets.length(); i++)
            if (!presets.getJSONObject(i).getString("summary").isEmpty())
                lastTiered = i;
        for (int i = 0; i < presets.length(); i++) {
            JSONObject preset = presets.getJSONObject(i);
            boolean tiered = !preset.getString("summary").isEmpty();
            MaterialCardView card = presetCard(preset);
            if (i == lastTiered)
                card.setTag("first");
            LinearLayout.LayoutParams place;
            if (tiered && wide()) {
                place = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1);
                place.setMargins(i == 0 ? 0 : dp(6), 0, i == lastTiered ? 0 : dp(6), 0);
                cards.addView(card, place);
                continue;
            }
            place = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            place.setMargins(0, dp(12), 0, 0);
            (tiered ? cards : content).addView(card, place);
        }
    }

    private MaterialCardView presetCard(JSONObject preset) throws JSONException {
        String id = preset.getString("id");
        MaterialCardView card = new MaterialCardView(ui, null, com.google.android.material.R.attr.materialCardViewElevatedStyle);
        card.setClickable(true);
        card.setFocusable(true);
        card.setOnClickListener(v -> nativeSetupControl("preset " + id));
        LinearLayout words = new LinearLayout(ui);
        words.setOrientation(LinearLayout.VERTICAL);
        words.setPadding(dp(16), dp(12), dp(16), dp(12));
        words.addView(text(preset.getString("label"), com.google.android.material.R.style.TextAppearance_Material3_TitleMedium,
                com.google.android.material.R.attr.colorOnSurface));
        TextView help = body(preset.getString("help"));
        help.setPadding(0, dp(4), 0, 0);
        words.addView(help, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        String summary = preset.getString("summary");
        if (!summary.isEmpty()) {
            TextView line = text(summary, com.google.android.material.R.style.TextAppearance_Material3_BodySmall,
                    com.google.android.material.R.attr.colorOnSurfaceVariant);
            line.setPadding(0, dp(8), 0, 0);
            words.addView(line);
        }
        card.addView(words, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return card;
    }

    // ---- a page of rows ----

    private void buildRows(JSONObject view) throws JSONException {
        String intro = view.getString("text");
        if (!intro.isEmpty()) {
            // A one-paragraph intro is secondary; a page explaining something at length is meant to be read.
            content.addView(intro.contains("\n\n") ? body(intro) : dim(intro));
        }
        if (view.getJSONArray("tiers").length() > 0)
            buildTiers(view);
        JSONArray items = view.getJSONArray("items");
        for (int i = 0; i < items.length(); i++) {
            JSONObject item = items.getJSONObject(i);
            Row row = item.getString("type").equals("group") ? new GroupRow(item) : makeOptionRow(item);
            rows.put(itemId(item), row);
            content.addView(row.view());
        }
    }

    private Row makeOptionRow(JSONObject item) throws JSONException {
        JSONObject row = item.getJSONObject("row");
        switch (row.getString("kind")) {
        case "toggle": return new ToggleRow(row);
        case "number": return new NumberRow(row);
        default: return item.getString("look").equals("slider") ? new ChoiceSliderRow(row) : new ChoiceRow(row);
        }
    }

    // "Set everything to", each tier a chip, the one every row is at checked.
    private void buildTiers(JSONObject view) throws JSONException {
        JSONArray tiers = view.getJSONArray("tiers");
        LinearLayout line = rowLayout();
        line.addView(rowTitle("Set everything to"), labelPlace());
        tierChips = new LinearLayout(ui);
        tierChips.setOrientation(LinearLayout.HORIZONTAL);
        for (int t = 0; t < tiers.length(); t++) {
            JSONObject tier = tiers.getJSONObject(t);
            String id = tier.getString("id");
            Chip chip = chip(tier.getString("label"));
            chip.setOnClickListener(v -> nativeSetupControl("tier " + id));
            LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            gap.setMarginEnd(dp(8));
            tierChips.addView(chip, gap);
        }
        line.addView(tierChips, controlPlace());
        content.addView(line);
    }

    private void showTiers(JSONObject view) throws JSONException {
        int lit = view.isNull("pageTier") ? -1 : view.getInt("pageTier");
        for (int t = 0; t < tierChips.getChildCount(); t++)
            ((Chip) tierChips.getChildAt(t)).setChecked(t == lit);
    }

    // A row's frame: the label beside a column of control and text, or over it on a narrow screen.
    private LinearLayout rowLayout() {
        LinearLayout layout = new LinearLayout(ui);
        layout.setOrientation(wide() ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        layout.setPadding(0, dp(10), 0, 0);
        return layout;
    }

    private LinearLayout.LayoutParams labelPlace() {
        if (wide())
            return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 3);
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private LinearLayout.LayoutParams controlPlace() {
        if (wide())
            return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 7);
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private TextView rowTitle(String label) {
        TextView title = text(label, com.google.android.material.R.style.TextAppearance_Material3_TitleSmall,
                com.google.android.material.R.attr.colorOnSurface);
        title.setPadding(0, wide() ? dp(14) : 0, dp(12), 0);
        return title;
    }

    // Answers as chips, or behind a button when there are too many for a short screen.
    private class Answers {
        final LinearLayout control = new LinearLayout(ui);
        final ChipGroup chips;
        final MaterialButton opener;
        final List<String> names;
        private final AnswerPicked picked;

        Answers(List<String> names, AnswerPicked picked) {
            this.names = names;
            this.picked = picked;
            control.setOrientation(LinearLayout.VERTICAL);
            if (names.size() <= MOST_CHIPS) {
                chips = new ChipGroup(ui);
                chips.setChipSpacingVertical(0);
                for (int i = 0; i < names.size(); i++) {
                    int index = i;
                    Chip chip = chip(names.get(i));
                    chip.setOnClickListener(v -> picked.at(index));
                    chips.addView(chip);
                }
                control.addView(chips);
                opener = null;
                return;
            }
            chips = null;
            opener = new MaterialButton(ui, null, com.google.android.material.R.attr.materialButtonOutlinedStyle);
            opener.setAllCaps(false);
            opener.setOnClickListener(v -> openList());
            control.addView(opener, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        void openList() {
            PopupMenu menu = new PopupMenu(ui, opener);
            for (int i = 0; i < names.size(); i++)
                menu.getMenu().add(0, i, i, names.get(i));
            menu.setOnMenuItemClickListener(item -> {
                picked.at(item.getItemId());
                return true;
            });
            menu.show();
        }

        void show(int chosen) {
            if (chips == null) {
                opener.setText(chosen >= 0 ? names.get(chosen) : "");
                return;
            }
            for (int i = 0; i < chips.getChildCount(); i++)
                ((Chip) chips.getChildAt(i)).setChecked(i == chosen);
        }
    }

    private interface AnswerPicked {
        void at(int index);
    }

    private class GroupRow implements Row {
        final LinearLayout layout = rowLayout();
        final String id;
        final boolean keepOffered;
        final JSONArray choices;
        final Answers answers;
        final TextView help;
        final TextView cost;

        GroupRow(JSONObject item) throws JSONException {
            id = item.getString("id");
            keepOffered = item.getBoolean("keepOffered");
            choices = item.getJSONArray("choices");
            List<String> names = new ArrayList<>();
            if (keepOffered)
                names.add(KEEP);
            for (int i = 0; i < choices.length(); i++)
                names.add(choices.getJSONObject(i).getString("label"));
            answers = new Answers(names, this::pick);
            layout.addView(rowTitle(item.getString("label")), labelPlace());
            LinearLayout control = new LinearLayout(ui);
            control.setOrientation(LinearLayout.VERTICAL);
            control.addView(answers.control);
            help = text("", com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorOnSurfaceVariant);
            control.addView(help);
            cost = text("Higher GPU cost.", com.google.android.material.R.style.TextAppearance_Material3_BodySmall,
                    com.google.android.material.R.attr.colorTertiary);
            control.addView(cost);
            layout.addView(control, controlPlace());
        }

        void pick(int index) {
            if (keepOffered && index == 0)
                nativeSetupControl("choose " + id + " keep");
            else
                nativeSetupControl("choose " + id + " " + (keepOffered ? index - 1 : index));
        }

        public View view() { return layout; }

        public void show(JSONObject item) throws JSONException {
            int chosen = item.isNull("chosen") ? -1 : item.getInt("chosen");
            answers.show(chosen < 0 ? (keepOffered ? 0 : -1) : chosen + (keepOffered ? 1 : 0));
            JSONObject choice = chosen >= 0 ? choices.getJSONObject(chosen) : null;
            String words = choice != null ? choice.getString("help") : KEEP_HELP;
            help.setText(words);
            help.setVisibility(words.isEmpty() ? View.GONE : View.VISIBLE);
            cost.setVisibility(choice != null && choice.getBoolean("gpuCost") ? View.VISIBLE : View.GONE);
        }
    }

    // An option's frame: its label, the control, its help under that.
    private void optionColumn(LinearLayout layout, JSONObject row, View control) throws JSONException {
        layout.addView(rowTitle(row.getString("label")), labelPlace());
        LinearLayout column = new LinearLayout(ui);
        column.setOrientation(LinearLayout.VERTICAL);
        column.addView(control);
        String help = row.getString("help");
        if (!help.isEmpty())
            column.addView(text(help, com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorOnSurfaceVariant));
        layout.addView(column, controlPlace());
    }

    private class ChoiceRow implements Row {
        final LinearLayout layout = rowLayout();
        final JSONArray choices;
        final Answers answers;

        ChoiceRow(JSONObject row) throws JSONException {
            String id = row.getString("id");
            choices = row.getJSONArray("choices");
            List<String> names = new ArrayList<>();
            for (int i = 0; i < choices.length(); i++)
                names.add(choices.getJSONObject(i).getString("label"));
            answers = new Answers(names, index -> sendChoice(id, choices, index));
            optionColumn(layout, row, answers.control);
        }

        public View view() { return layout; }

        public void show(JSONObject item) throws JSONException {
            answers.show(choiceAt(choices, item.getJSONObject("row").get("value")));
        }
    }

    // A slider whose positions are the row's choices, the chosen one's label beside it.
    private class ChoiceSliderRow implements Row {
        final LinearLayout layout = rowLayout();
        final String id;
        final JSONArray choices;
        final Slider slider = new Slider(ui);
        final TextView shown;

        ChoiceSliderRow(JSONObject row) throws JSONException {
            id = row.getString("id");
            choices = row.getJSONArray("choices");
            shown = text("", com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorPrimary);
            shown.setPadding(0, dp(14), 0, 0);
            LinearLayout control = new LinearLayout(ui);
            control.setOrientation(LinearLayout.VERTICAL);
            control.addView(shown);
            slider.setValueFrom(0);
            slider.setValueTo(Math.max(1, choices.length() - 1));
            slider.setStepSize(1);
            slider.setLabelBehavior(com.google.android.material.slider.LabelFormatter.LABEL_GONE);
            control.addView(slider, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            optionColumn(layout, row, control);
            slider.addOnChangeListener((s, at, fromUser) -> {
                if (!fromUser)
                    return;
                showLabel(Math.round(at));
                if (!id.equals(dragging))
                    sendChoice(id, choices, Math.round(at));   // a pad's D-pad, no finger to wait for
            });
            slider.addOnSliderTouchListener(new Slider.OnSliderTouchListener() {
                @Override
                public void onStartTrackingTouch(Slider s) {
                    dragging = id;
                }

                @Override
                public void onStopTrackingTouch(Slider s) {
                    dragging = null;
                    sendChoice(id, choices, Math.round(s.getValue()));
                }
            });
        }

        void showLabel(int at) {
            try {
                shown.setText(choices.getJSONObject(at).getString("label"));
            } catch (JSONException e) {
                Log.w(TAG, "setup: " + e);
            }
        }

        public View view() { return layout; }

        public void show(JSONObject item) throws JSONException {
            if (id.equals(dragging))
                return;
            int at = Math.max(0, choiceAt(choices, item.getJSONObject("row").get("value")));
            slider.setValue(at);
            showLabel(at);
        }
    }

    // A slider over whole steps from min, so the Material slider never sees a value off its step.
    private class NumberRow implements Row {
        final LinearLayout layout = rowLayout();
        final String id;
        final double min, max, step;
        final int steps;
        final String format;
        final Slider slider = new Slider(ui);
        final TextView shown;

        NumberRow(JSONObject row) throws JSONException {
            id = row.getString("id");
            min = row.getDouble("min");
            max = row.getDouble("max");
            double given = row.getDouble("step");
            step = given > 0 ? given : (max - min) / 100;
            steps = Math.max(1, (int) Math.round((max - min) / step));
            format = row.getString("format");
            shown = text("", com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorPrimary);
            shown.setPadding(0, dp(14), 0, 0);
            LinearLayout control = new LinearLayout(ui);
            control.setOrientation(LinearLayout.VERTICAL);
            control.addView(shown);
            slider.setValueFrom(0);
            slider.setValueTo(steps);
            slider.setStepSize(1);
            slider.setLabelBehavior(com.google.android.material.slider.LabelFormatter.LABEL_GONE);
            control.addView(slider, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            optionColumn(layout, row, control);
            slider.addOnChangeListener((s, at, fromUser) -> {
                if (!fromUser)
                    return;
                shown.setText(words(value(at)));
                if (!id.equals(dragging))
                    send(value(at));
            });
            slider.addOnSliderTouchListener(new Slider.OnSliderTouchListener() {
                @Override
                public void onStartTrackingTouch(Slider s) {
                    dragging = id;
                }

                @Override
                public void onStopTrackingTouch(Slider s) {
                    dragging = null;
                    send(value(s.getValue()));
                }
            });
        }

        void send(double value) {
            nativeSetupControl("option " + id + " " + value);
        }

        double value(float at) {
            return Math.min(max, min + Math.round(at) * step);
        }

        String words(double value) {
            try {
                return String.format(Locale.ROOT, format.isEmpty() ? "%.2f" : format, value);
            } catch (IllegalFormatException e) {
                return Double.toString(value);
            }
        }

        public View view() { return layout; }

        public void show(JSONObject item) throws JSONException {
            if (id.equals(dragging))
                return;
            double value = item.getJSONObject("row").getDouble("value");
            slider.setValue(Math.max(0, Math.min(steps, Math.round((value - min) / step))));
            shown.setText(words(value));
        }
    }

    private class ToggleRow implements Row {
        final LinearLayout layout = rowLayout();
        final MaterialSwitch toggle = new MaterialSwitch(ui);
        boolean updating;

        ToggleRow(JSONObject row) throws JSONException {
            String id = row.getString("id");
            optionColumn(layout, row, toggle);
            toggle.setOnCheckedChangeListener((button, on) -> {
                if (!updating)
                    nativeSetupControl("option " + id + " " + (on ? "true" : "false"));
            });
        }

        public View view() { return layout; }

        public void show(JSONObject item) throws JSONException {
            updating = true;
            toggle.setChecked(item.getJSONObject("row").getBoolean("value"));
            updating = false;
        }
    }

    // ---- the summary ----

    // Page by page, in two columns where they fit, so a short screen scrolls less.
    private void buildSummary(JSONObject view) throws JSONException {
        content.addView(dim("These choices will be applied:"));
        LinearLayout columns = new LinearLayout(ui);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        int count = wide() ? 2 : 1;
        LinearLayout[] each = new LinearLayout[count];
        int[] lines = new int[count];
        for (int c = 0; c < count; c++) {
            each[c] = new LinearLayout(ui);
            each[c].setOrientation(LinearLayout.VERTICAL);
            each[c].setPadding(c > 0 ? dp(16) : 0, 0, c + 1 < count ? dp(16) : 0, 0);
            columns.addView(each[c], new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        }
        JSONArray sections = view.getJSONArray("summary");
        for (int s = 0; s < sections.length(); s++) {
            int c = count == 2 && lines[1] < lines[0] ? 1 : 0;
            lines[c] += addSummarySection(each[c], sections.getJSONObject(s));
        }
        content.addView(columns);
    }

    private int addSummarySection(LinearLayout into, JSONObject section) throws JSONException {
        TextView heading = text(section.getString("title"), com.google.android.material.R.style.TextAppearance_Material3_LabelLarge,
                com.google.android.material.R.attr.colorPrimary);
        heading.setPadding(0, dp(12), 0, dp(2));
        into.addView(heading);
        JSONArray lines = section.getJSONArray("lines");
        for (int l = 0; l < lines.length(); l++) {
            JSONObject line = lines.getJSONObject(l);
            LinearLayout row = new LinearLayout(ui);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setPadding(0, dp(2), 0, dp(2));
            row.addView(text(line.getString("label"), com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorOnSurfaceVariant),
                    new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 2));
            row.addView(text(line.getString("value"), com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorOnSurface),
                    new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 3));
            into.addView(row);
        }
        return lines.length() + 1;
    }

    // ---- helpers ----

    private void sendChoice(String id, JSONArray choices, int index) {
        try {
            nativeSetupControl("option " + id + " " + valueText(choices.getJSONObject(index).get("value")));
        } catch (JSONException e) {
            Log.w(TAG, "setup: " + e);
        }
    }

    private static int choiceAt(JSONArray choices, Object value) throws JSONException {
        String wanted = valueText(value);
        for (int i = 0; i < choices.length(); i++)
            if (valueText(choices.getJSONObject(i).get("value")).equals(wanted))
                return i;
        return -1;
    }

    // A value as the game reads it back from text (verdite::option_value_from_text).
    private static String valueText(Object value) {
        if (value instanceof Boolean)
            return (Boolean) value ? "true" : "false";
        if (value instanceof Number) {
            double number = ((Number) value).doubleValue();
            if (number == Math.rint(number) && Math.abs(number) < 1e15)
                return Long.toString((long) number);
            return Double.toString(number);
        }
        return String.valueOf(value);
    }

    private boolean wide() {
        return activity.getResources().getConfiguration().screenWidthDp >= WIDE_DP;
    }

    private Chip chip(String label) {
        Chip chip = new Chip(ui);
        chip.setText(label);
        chip.setCheckable(true);
        chip.setCheckedIconVisible(false);
        chip.setFocusable(true);
        return chip;
    }

    private TextView body(String words) {
        return text(words, com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                com.google.android.material.R.attr.colorOnSurface);
    }

    private TextView dim(String words) {
        return text(words, com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                com.google.android.material.R.attr.colorOnSurfaceVariant);
    }

    private TextView text(String words, int appearance, int colorAttr) {
        TextView view = new TextView(ui);
        view.setTextAppearance(appearance);
        view.setTextColor(color(colorAttr));
        view.setText(words);
        return view;
    }

    private int color(int attr) {
        return MaterialColors.getColor(ui, attr, 0xFF808080);
    }

    private int dp(float value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, activity.getResources().getDisplayMetrics()));
    }
}
