package io.github.voicedrew11.verdite1;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;

import androidx.appcompat.view.ContextThemeWrapper;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

// SDL's activity, which loads libSDL3.so and libmain.so and runs the game's main on its own thread.
public class VerditeActivity extends SDLActivity {
    private static final int DOWNLOAD_TRIES = 3;
    private static final int CONNECT_TIMEOUT_MS = 20_000;
    private static final int READ_TIMEOUT_MS = 60_000;

    private SettingsPanel settings;
    private SetupPanel setup;

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

    static native void nativeUpdateSkip();

    // From the game thread (port/ext/update_notice.cpp): a new release, told once, at the title.
    public void showUpdateNotice(String available, String running, String pageUrl) {
        runOnUiThread(() -> new MaterialAlertDialogBuilder(new ContextThemeWrapper(this, R.style.Theme_Verdite_Settings))
                .setTitle("Update available")
                .setMessage(available + "\n" + running)
                .setPositiveButton("Download page", (dialog, which) -> openPage(pageUrl))
                .setNegativeButton("Skip this version", (dialog, which) -> nativeUpdateSkip())
                .setNeutralButton("Later", null)
                .setOnDismissListener(dialog -> mSurface.requestFocus())
                .show());
    }

    private void openPage(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            Log.w("Verdite1", "no browser for " + url + ": " + e);
        }
    }

    // From the game thread: the settings' values or rows changed.
    public void onOptionsChanged() {
        runOnUiThread(() -> {
            if (settings != null)
                settings.refresh();
        });
    }

    // From the game thread (port/ext/setup_panel.cpp): a new view of the setup, the first opening it.
    public void showSetupPanel() {
        runOnUiThread(() -> {
            if (setup == null)
                setup = new SetupPanel(this, mLayout, () -> mSurface.requestFocus());
            setup.show();
        });
    }

    public void closeSetupPanel() {
        runOnUiThread(() -> {
            if (setup != null)
                setup.close();
        });
    }

    // From the game thread (port/platform/touch.cpp): a button on the screen went down. The
    // phone's own key click, as the system's "touch feedback" setting allows.
    public void feelButtonPress() {
        runOnUiThread(() -> mSurface.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY));
    }

    // From a language task's thread or the game's (port/platform/patch_download_android.cpp): the
    // file at an https URL, or null. The game checks what it gets.
    public byte[] downloadFile(String url, int mostBytes) {
        for (int attempt = 1; attempt <= DOWNLOAD_TRIES; attempt++) {
            try {
                return fetch(url, mostBytes);
            } catch (IOException e) {
                Log.w("Verdite1", "download, try " + attempt + ": " + e);
            }
        }
        return null;
    }

    private static byte[] fetch(String url, int mostBytes) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        try {
            connection.setConnectTimeout(CONNECT_TIMEOUT_MS);
            connection.setReadTimeout(READ_TIMEOUT_MS);
            int status = connection.getResponseCode();
            if (status != HttpURLConnection.HTTP_OK)
                throw new IOException("HTTP " + status);
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            byte[] chunk = new byte[64 * 1024];
            try (InputStream in = connection.getInputStream()) {
                for (int got = in.read(chunk); got >= 0; got = in.read(chunk)) {
                    if (bytes.size() + got > mostBytes)
                        throw new IOException("larger than " + mostBytes + " bytes");
                    bytes.write(chunk, 0, got);
                }
            }
            return bytes.toByteArray();
        } finally {
            connection.disconnect();
        }
    }

    // A pad's Start goes on through the setup, as it does on a desktop.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (setup != null && setup.isOpen() && event.getKeyCode() == KeyEvent.KEYCODE_BUTTON_START) {
            if (event.getAction() == KeyEvent.ACTION_UP)
                setup.forward();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    // Back (and a pad's B) steps back through the setup or closes the settings; otherwise SDL's,
    // where the game takes it.
    @Override
    public void onBackPressed() {
        if (setup != null && setup.isOpen()) {
            setup.back();
            return;
        }
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
