package io.github.voicedrew11.verdite1;

import android.app.Activity;
import android.content.Context;
import android.content.res.ColorStateList;
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

import androidx.appcompat.widget.PopupMenu;
import androidx.appcompat.view.ContextThemeWrapper;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.color.DynamicColors;
import com.google.android.material.color.MaterialColors;
import com.google.android.material.materialswitch.MaterialSwitch;
import com.google.android.material.slider.Slider;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.IllegalFormatException;
import java.util.Locale;
import java.util.Map;

// The game's settings as Material 3 views, in a sheet over the live game. The
// rows come from the game's option table (port/ext/options.cpp) as JSON; a change
// goes back as text and is applied on the game thread, which then sends the new
// values, and the rows update in place. On the UI thread throughout.
final class SettingsPanel {
    private static final String TAG = "Verdite1";

    static native String nativeOptionsJson();
    static native void nativeOptionChange(String id, String text);
    static native void nativeClosed(boolean openOverlay);

    // One row of a page, built once and shown again with each new value.
    private interface Row {
        View view();
        void show(JSONObject row) throws JSONException;
    }

    private final Activity activity;
    private final ViewGroup host;           // SDL's layout, over its surface
    private final Runnable giveBackFocus;   // to SDL's surface, so keys and pads reach the game
    private final Context ui;               // the Material theme
    private FrameLayout root;
    private LinearLayout sheet, rail, content;
    private ScrollView scroll;
    private String page = "";
    private String builtTitles = "", builtIds = "";
    private final Map<String, Row> rows = new HashMap<>();
    private String dragging;                // a slider under a finger is not moved by updates
    private boolean open;

    SettingsPanel(Activity activity, ViewGroup host, Runnable giveBackFocus) {
        this.activity = activity;
        this.host = host;
        this.giveBackFocus = giveBackFocus;
        this.ui = DynamicColors.wrapContextIfAvailable(new ContextThemeWrapper(activity, R.style.Theme_Verdite_Settings));
    }

    boolean isOpen() {
        return open;
    }

    void show() {
        if (root == null)
            build();
        if (root.getParent() == null)
            host.addView(root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        open = true;
        root.setVisibility(View.VISIBLE);
        refresh();
        int width = sheetWidth();
        sheet.getLayoutParams().width = width;
        sheet.requestLayout();
        sheet.setTranslationX(width);
        sheet.animate().translationX(0).setDuration(180).start();
        root.requestApplyInsets();
        if (rail.getChildCount() > 1)
            rail.getChildAt(1).requestFocus();
    }

    void close(boolean openOverlay) {
        if (!open)
            return;
        open = false;
        dragging = null;
        sheet.animate().translationX(sheet.getWidth()).setDuration(150).withEndAction(() -> {
            if (!open)
                root.setVisibility(View.GONE);
        }).start();
        nativeClosed(openOverlay);
        giveBackFocus.run();
    }

    // The game's view again: the same rows updated in place, or the page built anew
    // when rows came or went.
    void refresh() {
        if (!open)
            return;
        try {
            JSONArray pages = new JSONObject(nativeOptionsJson()).getJSONArray("pages");
            showRail(pages);
            JSONObject shown = pageNamed(pages, page);
            if (shown == null)
                return;
            page = shown.getString("title");
            String ids = idsOf(shown);
            if (!ids.equals(builtIds))
                buildPage(shown, ids);
            for (JSONObject row : rowsOf(shown)) {
                Row built = rows.get(row.getString("id"));
                if (built != null)
                    built.show(row);
            }
        } catch (JSONException e) {
            Log.w(TAG, "settings: " + e);
        }
    }

    // ---- the sheet ----

    private void build() {
        root = new FrameLayout(activity);
        root.setClickable(true);   // a tap on the game beside the sheet closes it
        root.setOnClickListener(v -> close(false));
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            fitCutout(insets);
            return insets;
        });

        sheet = new LinearLayout(ui);
        sheet.setOrientation(LinearLayout.HORIZONTAL);
        sheet.setBackgroundColor(color(com.google.android.material.R.attr.colorSurfaceContainer));
        sheet.setElevation(dp(8));
        sheet.setClickable(true);   // touches on the sheet stay on it
        root.addView(sheet, new FrameLayout.LayoutParams(sheetWidth(), ViewGroup.LayoutParams.MATCH_PARENT, Gravity.END));

        ScrollView railScroll = new ScrollView(ui);
        rail = new LinearLayout(ui);
        rail.setOrientation(LinearLayout.VERTICAL);
        rail.setPadding(dp(12), dp(16), dp(12), dp(16));
        railScroll.addView(rail);
        sheet.addView(railScroll, new LinearLayout.LayoutParams(dp(168), ViewGroup.LayoutParams.MATCH_PARENT));

        scroll = new ScrollView(ui);
        scroll.setBackgroundColor(color(com.google.android.material.R.attr.colorSurface));
        content = new LinearLayout(ui);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(8), dp(8), dp(8), dp(32));
        scroll.addView(content);
        sheet.addView(scroll, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1));
    }

    private int sheetWidth() {
        int width = host.getWidth() > 0 ? host.getWidth() : activity.getResources().getDisplayMetrics().widthPixels;
        return Math.min(width, Math.min(Math.max(width * 62 / 100, dp(480)), dp(860)));
    }

    // A cutout on the sheet's side, in landscape, would cover its edge.
    private void fitCutout(WindowInsets insets) {
        DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout == null)
            return;
        sheet.setPadding(0, cutout.getSafeInsetTop(), cutout.getSafeInsetRight(), cutout.getSafeInsetBottom());
    }

    private void showRail(JSONArray pages) throws JSONException {
        StringBuilder titles = new StringBuilder();
        for (int i = 0; i < pages.length(); i++)
            titles.append(pages.getJSONObject(i).getString("title")).append('\n');
        if (titles.toString().equals(builtTitles)) {
            markPage();
            return;
        }
        builtTitles = titles.toString();
        rail.removeAllViews();
        TextView heading = text("Settings", com.google.android.material.R.style.TextAppearance_Material3_TitleLarge,
                com.google.android.material.R.attr.colorOnSurface);
        heading.setPadding(dp(12), dp(4), dp(12), dp(16));
        rail.addView(heading);
        for (int i = 0; i < pages.length(); i++) {
            String title = pages.getJSONObject(i).getString("title");
            MaterialButton button = railButton(title);
            button.setTag(title);
            button.setOnClickListener(v -> {
                page = title;
                builtIds = "";
                scroll.scrollTo(0, 0);
                refresh();
            });
            rail.addView(button);
        }
        View gap = new View(ui);
        rail.addView(gap, new LinearLayout.LayoutParams(1, dp(24)));
        MaterialButton overlay = railButton("Mods and console");
        overlay.setOnClickListener(v -> close(true));
        rail.addView(overlay);
        MaterialButton done = railButton("Close");
        done.setOnClickListener(v -> close(false));
        rail.addView(done);
        markPage();
    }

    private MaterialButton railButton(String title) {
        MaterialButton button = new MaterialButton(ui, null, com.google.android.material.R.attr.borderlessButtonStyle);
        button.setText(title);
        button.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        button.setAllCaps(false);
        button.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return button;
    }

    private void markPage() {
        int selected = color(com.google.android.material.R.attr.colorSecondaryContainer);
        int onSelected = color(com.google.android.material.R.attr.colorOnSecondaryContainer);
        int plain = color(com.google.android.material.R.attr.colorOnSurfaceVariant);
        for (int i = 0; i < rail.getChildCount(); i++) {
            View child = rail.getChildAt(i);
            if (!(child instanceof MaterialButton) || child.getTag() == null)
                continue;
            MaterialButton button = (MaterialButton) child;
            boolean on = page.equals(button.getTag());
            button.setBackgroundTintList(ColorStateList.valueOf(on ? selected : 0));
            button.setTextColor(on ? onSelected : plain);
        }
    }

    private void buildPage(JSONObject shown, String ids) throws JSONException {
        int at = scroll.getScrollY();
        content.removeAllViews();
        rows.clear();
        JSONArray sections = shown.getJSONArray("sections");
        for (int s = 0; s < sections.length(); s++) {
            JSONObject section = sections.getJSONObject(s);
            String title = section.getString("title");
            if (!title.isEmpty()) {
                TextView heading = text(title, com.google.android.material.R.style.TextAppearance_Material3_LabelLarge,
                        com.google.android.material.R.attr.colorPrimary);
                heading.setPadding(dp(16), dp(24), dp(16), dp(4));
                content.addView(heading);
            }
            JSONArray sectionRows = section.getJSONArray("rows");
            for (int r = 0; r < sectionRows.length(); r++) {
                JSONObject row = sectionRows.getJSONObject(r);
                Row built = makeRow(row);
                rows.put(row.getString("id"), built);
                content.addView(built.view());
            }
        }
        builtIds = ids;
        markPage();
        scroll.post(() -> scroll.scrollTo(0, at));
    }

    private Row makeRow(JSONObject row) throws JSONException {
        switch (row.getString("kind")) {
        case "toggle": return new ToggleRow(row);
        case "choice": return new ChoiceRow(row);
        case "number": return new NumberRow(row);
        case "action": return new ActionRow(row);
        default: return new StatusRow(row);
        }
    }

    // ---- rows ----

    // A row's title, its help (two lines until tapped) and its note.
    private class Texts {
        final LinearLayout column = new LinearLayout(ui);
        final TextView title, help, note;

        Texts(JSONObject row) throws JSONException {
            column.setOrientation(LinearLayout.VERTICAL);
            title = text(row.getString("label"), com.google.android.material.R.style.TextAppearance_Material3_BodyLarge,
                    com.google.android.material.R.attr.colorOnSurface);
            column.addView(title);
            help = text(row.getString("help"), com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorOnSurfaceVariant);
            help.setMaxLines(2);
            help.setEllipsize(android.text.TextUtils.TruncateAt.END);
            help.setOnClickListener(v -> help.setMaxLines(help.getMaxLines() == 2 ? Integer.MAX_VALUE : 2));
            help.setVisibility(row.getString("help").isEmpty() ? View.GONE : View.VISIBLE);
            column.addView(help);
            note = text("", com.google.android.material.R.style.TextAppearance_Material3_BodySmall,
                    com.google.android.material.R.attr.colorTertiary);
            column.addView(note);
        }

        void show(JSONObject row) throws JSONException {
            boolean enabled = row.getBoolean("enabled");
            column.setAlpha(enabled ? 1f : 0.38f);
            String text = row.getString("note");
            note.setText(text);
            note.setVisibility(text.isEmpty() ? View.GONE : View.VISIBLE);
        }
    }

    private LinearLayout rowLayout(int orientation) {
        LinearLayout layout = new LinearLayout(ui);
        layout.setOrientation(orientation);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        layout.setMinimumHeight(dp(56));
        layout.setPadding(dp(16), dp(10), dp(16), dp(10));
        return layout;
    }

    private void clickable(View view) {
        TypedValue ripple = new TypedValue();
        ui.getTheme().resolveAttribute(android.R.attr.selectableItemBackground, ripple, true);
        view.setBackgroundResource(ripple.resourceId);
        view.setFocusable(true);
        view.setClickable(true);
    }

    private class ToggleRow implements Row {
        final String id;
        final LinearLayout layout = rowLayout(LinearLayout.HORIZONTAL);
        final Texts texts;
        final MaterialSwitch toggle = new MaterialSwitch(ui);
        boolean updating;

        ToggleRow(JSONObject row) throws JSONException {
            id = row.getString("id");
            texts = new Texts(row);
            layout.addView(texts.column, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            toggle.setFocusable(false);   // the row takes the focus and the tap
            layout.addView(toggle);
            clickable(layout);
            layout.setOnClickListener(v -> toggle.toggle());
            toggle.setOnCheckedChangeListener((button, on) -> {
                if (!updating)
                    nativeOptionChange(id, on ? "true" : "false");
            });
        }

        public View view() { return layout; }

        public void show(JSONObject row) throws JSONException {
            texts.show(row);
            boolean enabled = row.getBoolean("enabled");
            layout.setEnabled(enabled);
            toggle.setEnabled(enabled);
            updating = true;
            toggle.setChecked(row.getBoolean("value"));
            updating = false;
        }
    }

    private class ChoiceRow implements Row {
        final String id;
        final LinearLayout layout = rowLayout(LinearLayout.VERTICAL);
        final Texts texts;
        final TextView chosen;
        final JSONArray choices;

        ChoiceRow(JSONObject row) throws JSONException {
            id = row.getString("id");
            choices = row.getJSONArray("choices");
            layout.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            texts = new Texts(row);
            chosen = text("", com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorPrimary);
            texts.column.addView(chosen, 1);
            layout.addView(texts.column, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            clickable(layout);
            layout.setOnClickListener(v -> pick());
        }

        void pick() {
            PopupMenu menu = new PopupMenu(ui, chosen);
            try {
                for (int i = 0; i < choices.length(); i++)
                    menu.getMenu().add(0, i, i, choices.getJSONObject(i).getString("label"));
            } catch (JSONException e) {
                Log.w(TAG, "settings: " + e);
                return;
            }
            menu.setOnMenuItemClickListener(item -> {
                try {
                    nativeOptionChange(id, valueText(choices.getJSONObject(item.getItemId()).get("value")));
                } catch (JSONException e) {
                    Log.w(TAG, "settings: " + e);
                }
                return true;
            });
            menu.show();
        }

        public View view() { return layout; }

        public void show(JSONObject row) throws JSONException {
            texts.show(row);
            layout.setEnabled(row.getBoolean("enabled"));
            String value = valueText(row.get("value"));
            chosen.setText("");
            for (int i = 0; i < choices.length(); i++)
                if (valueText(choices.getJSONObject(i).get("value")).equals(value))
                    chosen.setText(choices.getJSONObject(i).getString("label"));
        }
    }

    // A slider over whole steps from min, so the Material slider never sees a
    // value off its step.
    private class NumberRow implements Row {
        final String id;
        final LinearLayout layout = rowLayout(LinearLayout.VERTICAL);
        final Texts texts;
        final TextView shownValue;
        final Slider slider = new Slider(ui);
        final double min, max, step;
        final int steps;
        final String format;
        final boolean settle;

        NumberRow(JSONObject row) throws JSONException {
            id = row.getString("id");
            min = row.getDouble("min");
            max = row.getDouble("max");
            double given = row.getDouble("step");
            step = given > 0 ? given : (max - min) / 100;
            steps = Math.max(1, (int) Math.round((max - min) / step));
            format = row.getString("format");
            settle = row.getBoolean("settle");
            layout.setGravity(Gravity.START);
            texts = new Texts(row);
            LinearLayout top = new LinearLayout(ui);
            top.setOrientation(LinearLayout.HORIZONTAL);
            texts.column.removeView(texts.title);
            top.addView(texts.title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            shownValue = text("", com.google.android.material.R.style.TextAppearance_Material3_BodyLarge,
                    com.google.android.material.R.attr.colorPrimary);
            top.addView(shownValue);
            texts.column.addView(top, 0);
            layout.addView(texts.column, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            slider.setValueFrom(0);
            slider.setValueTo(steps);
            slider.setStepSize(1);
            slider.setLabelFormatter(at -> shown(value(at)));
            slider.addOnChangeListener((s, at, fromUser) -> {
                if (!fromUser)
                    return;
                shownValue.setText(shown(value(at)));
                if (!settle)
                    nativeOptionChange(id, Double.toString(value(at)));
            });
            slider.addOnSliderTouchListener(new Slider.OnSliderTouchListener() {
                @Override
                public void onStartTrackingTouch(Slider s) {
                    dragging = id;
                }

                @Override
                public void onStopTrackingTouch(Slider s) {
                    dragging = null;
                    if (settle)
                        nativeOptionChange(id, Double.toString(value(s.getValue())));
                }
            });
            layout.addView(slider, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        double value(float at) {
            return Math.min(max, min + Math.round(at) * step);
        }

        String shown(double value) {
            try {
                return String.format(Locale.ROOT, format.isEmpty() ? "%.2f" : format, value);
            } catch (IllegalFormatException e) {
                return Double.toString(value);
            }
        }

        public View view() { return layout; }

        public void show(JSONObject row) throws JSONException {
            texts.show(row);
            slider.setEnabled(row.getBoolean("enabled"));
            if (id.equals(dragging))
                return;
            double value = row.getDouble("value");
            long at = Math.round((value - min) / step);
            slider.setValue(Math.max(0, Math.min(steps, at)));
            shownValue.setText(shown(value));
        }
    }

    private class ActionRow implements Row {
        final LinearLayout layout = rowLayout(LinearLayout.VERTICAL);
        final Texts texts;
        final MaterialButton button;

        ActionRow(JSONObject row) throws JSONException {
            String id = row.getString("id");
            layout.setGravity(Gravity.START);
            texts = new Texts(row);
            texts.column.removeView(texts.title);
            button = new MaterialButton(ui, null, com.google.android.material.R.attr.materialButtonOutlinedStyle);
            button.setText(row.getString("label"));
            button.setAllCaps(false);
            button.setOnClickListener(v -> nativeOptionChange(id, ""));
            layout.addView(button);
            layout.addView(texts.column);
        }

        public View view() { return layout; }

        public void show(JSONObject row) throws JSONException {
            texts.show(row);
            button.setEnabled(row.getBoolean("enabled"));
        }
    }

    // Text from the game: a label and its value side by side, or a line of its own.
    private class StatusRow implements Row {
        final LinearLayout layout = rowLayout(LinearLayout.HORIZONTAL);
        final TextView label, value;

        StatusRow(JSONObject row) throws JSONException {
            layout.setMinimumHeight(dp(36));
            String title = row.getString("label");
            label = text(title, com.google.android.material.R.style.TextAppearance_Material3_BodyLarge,
                    com.google.android.material.R.attr.colorOnSurface);
            label.setVisibility(title.isEmpty() ? View.GONE : View.VISIBLE);
            layout.addView(label, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
            value = text("", com.google.android.material.R.style.TextAppearance_Material3_BodyMedium,
                    com.google.android.material.R.attr.colorOnSurfaceVariant);
            layout.addView(value, new LinearLayout.LayoutParams(title.isEmpty() ? 0 : ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT, title.isEmpty() ? 1 : 0));
        }

        public View view() { return layout; }

        public void show(JSONObject row) throws JSONException {
            value.setText(row.optString("value"));
        }
    }

    // ---- helpers ----

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

    private static JSONObject pageNamed(JSONArray pages, String title) throws JSONException {
        for (int i = 0; i < pages.length(); i++)
            if (pages.getJSONObject(i).getString("title").equals(title))
                return pages.getJSONObject(i);
        return pages.length() > 0 ? pages.getJSONObject(0) : null;
    }

    private static java.util.List<JSONObject> rowsOf(JSONObject page) throws JSONException {
        java.util.List<JSONObject> all = new java.util.ArrayList<>();
        JSONArray sections = page.getJSONArray("sections");
        for (int s = 0; s < sections.length(); s++) {
            JSONArray sectionRows = sections.getJSONObject(s).getJSONArray("rows");
            for (int r = 0; r < sectionRows.length(); r++)
                all.add(sectionRows.getJSONObject(r));
        }
        return all;
    }

    private static String idsOf(JSONObject page) throws JSONException {
        StringBuilder ids = new StringBuilder();
        for (JSONObject row : rowsOf(page))
            ids.append(row.getString("id")).append('\n');
        return ids.toString();
    }

    private TextView text(String text, int appearance, int colorAttr) {
        TextView view = new TextView(ui);
        view.setTextAppearance(appearance);
        view.setTextColor(color(colorAttr));
        view.setText(text);
        return view;
    }

    private int color(int attr) {
        return MaterialColors.getColor(ui, attr, 0xFF808080);
    }

    private int dp(float value) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, activity.getResources().getDisplayMetrics()));
    }
}
