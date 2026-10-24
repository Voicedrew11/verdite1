King's Field Enhanced
=====================

King's Field (Japan, 1994) for Windows, in English with the fan translation
or in the original Japanese, plus optional widescreen, high frame rates,
lighting, an automap, quick saves, a settings overlay and Lua mods.

Project page, updates and bug reports:
  https://github.com/Acranon/kf1-enhanced

This is a pre-release. Please attach kf.log (see below) to bug reports.


WHAT YOU NEED
-------------
A disc image of King's Field (Japan) SLPS-00017 that you dumped yourself:
  - the original Japanese disc (the game offers to add the English translation
    by downloading the translator's own patch), or
  - a disc with the English translation patch (v1.0, by John Osborne) applied:
    https://www.romhacking.net/translations/7308/
Use a .cue + .bin pair (single track) or an .iso. No game files are included.


FIRST LAUNCH
------------
Run kings-field.exe and choose your disc image when asked. The game copies its
files into the "data" folder here, once. The image itself is not changed.
You can also drop the .cue onto kings-field.exe, or put it in this folder first.

To switch to a different image later, delete the "data" folder.


LANGUAGES
---------
English and Japanese can be switched at any time: in the game's Options menu
(Language row) or in F1 > Settings > Language. Saves work in both.
- English from a Japanese disc: the game downloads the translator's original
  patch (Kings_Field_Jap_to_Eng_v1.0.rar, about 6 MB) from the Internet
  Archive, or uses that file if you choose it or drop it onto kings-field.exe.
- Japanese with a patched disc: add your original Japanese disc image in
  F1 > Settings > Language, or drop it onto kings-field.exe.


CONTROLS
--------
  Move / strafe ........ W A S D
  Look ................. Mouse (click the window first), Page Up / Page Down
  Turn ................. Left / Right arrows
  Run .................. Shift
  Attack ............... Space, left mouse button
  Magic ................ Q, right mouse button
  Interact / confirm ... E, Enter
  Menu ................. Tab (Tab or Backspace backs out)
  Pause ................ P, Escape
  Full map ............. M
  Quick save / load .... F2 / F4 (F4 also works on the title screen)
  Settings overlay ..... F1
  Fullscreen ........... F11, Alt+Enter

Every enhancement can be switched off in the F1 settings, or all at once with
Classic mode at the top of F1 > Settings (your own settings come back when you
turn it off).


FILES
-----
Saves, settings, mods and kf.log are in:
  %APPDATA%\KingsField\SLPS00017
The example mods in the "mods" folder here are copied there on the first launch,
switched off. Enable them in F1 > Mods. See MODDING.md for writing your own.


CREDITS
-------
Built on sushi-shi's King's Field source port (CC0):
  https://github.com/sushi-shi/kings-field-decomp
Menus, mods and console inspired by Verdite2 by Voicedrew11 (MIT):
  https://github.com/Voicedrew11/verdite2
English translation by John Osborne (weissvulf / HwitVlf); it is applied to
your own disc, not included.
Uses Dear ImGui, Lua, cJSON and SDL3; their
licenses are in the "licenses" folder.

King's Field is by FromSoftware. This is an unofficial fan project, not
affiliated with or endorsed by FromSoftware or Sony.
