package io.github.voicedrew11.verdite1;

import org.libsdl.app.SDLActivity;

// SDL's activity, which loads libSDL3.so and libmain.so and runs the game's main on its own thread.
public class VerditeActivity extends SDLActivity {
    // The command line, for agents and tests:
    // adb shell am start -n io.github.voicedrew11.verdite1/.VerditeActivity -e args "--skip-intro"
    @Override
    protected String[] getArguments() {
        String args = getIntent().getStringExtra("args");
        if (args == null || args.trim().isEmpty())
            return new String[0];
        return args.trim().split("\\s+");
    }
}
