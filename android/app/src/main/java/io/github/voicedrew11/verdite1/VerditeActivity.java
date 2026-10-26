package io.github.voicedrew11.verdite1;

import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.HapticFeedbackConstants;

import org.libsdl.app.SDLActivity;

// SDL's activity, which loads libSDL3.so and libmain.so and runs the game's main on its own thread.
public class VerditeActivity extends SDLActivity {
    private SettingsPanel settings;

    // The command line and environment, for agents and tests:
    // adb shell am start -n io.github.voicedrew11.verdite1/.VerditeActivity -e args "--skip-intro" -e env "KF_SHELL=1"
    // SDL asks for the arguments on the game's thread just before main, so the environment is set by then.
    @Override
    protected String[] getArguments() {
        for (String entry : words(getIntent().getStringExtra("env"))) {
            int equals = entry.indexOf('=');
            if (equals <= 0)
                continue;
            try {
                Os.setenv(entry.substring(0, equals), entry.substring(equals + 1), true);
            } catch (ErrnoException e) {
                Log.w("Verdite1", "cannot set " + entry + ": " + e);
            }
        }
        return words(getIntent().getStringExtra("args"));
    }

    // From the game thread (port/ext/settings_panel.cpp); the panel lives on the UI thread.
    public void showSettingsPanel() {
        runOnUiThread(() -> {
            if (settings == null)
                settings = new SettingsPanel(this, mLayout, () -> mSurface.requestFocus());
            settings.show();
        });
    }

    // From the game thread: the settings' values or rows changed.
    public void onOptionsChanged() {
        runOnUiThread(() -> {
            if (settings != null)
                settings.refresh();
        });
    }

    // From the game thread (port/platform/touch.cpp): a button on the screen went down. The
    // phone's own key click, as the system's "touch feedback" setting allows.
    public void feelButtonPress() {
        runOnUiThread(() -> mSurface.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY));
    }

    // Back (and a pad's B) closes the panel; otherwise SDL's, where the game takes it.
    @Override
    public void onBackPressed() {
        if (settings != null && settings.isOpen()) {
            settings.close(false);
            return;
        }
        super.onBackPressed();
    }

    private static String[] words(String text) {
        if (text == null || text.trim().isEmpty())
            return new String[0];
        return text.trim().split("\\s+");
    }
}
