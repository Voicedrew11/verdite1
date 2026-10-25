# Android plan

The plan for the Android port, drawn up 2026-10-25, before the 3DS. It is a
draft: the open questions at the end are the user's to decide before the
steps that depend on them.

Android is the fourth platform the Vulkan backend reaches ([RENDERER_PLAN.md](RENDERER_PLAN.md)),
so this is not a new renderer. It is the existing Vulkan backend, made to run
on mobile drivers and tiled GPUs, plus the platform layer Android needs and touch
controls.

## Devices

Decided 2026-10-25: the game must play on the Retroid Pocket handhelds as well
as phones. The user's phone is a Pixel 8 Pro. Specs below are from retailer and
review pages (retrocatalog.com, droix.net), checked 2026-10-25; each one is to be
confirmed with `vulkaninfo` on the device before relying on it.

| device | SoC, GPU | Android | screen | role |
|---|---|---|---|---|
| Pixel 8 Pro | Tensor G3, Mali-G715 | 14+ | 2992 x 1344, ~20:9, portrait-native | the user's phone; touch controls; pre-rotation |
| Retroid Pocket 2S | Unisoc T610, Mali-G52 MC2 | 11 | 640 x 480, 4:3 | **the floor**: the weakest GPU and the oldest driver that must work |
| Retroid Pocket 3+ | Unisoc T618, Mali-G52 | 11 (to confirm) | 16:9 | same GPU class as the 2S |
| Retroid Pocket 4 Pro | Dimensity 1100, Mali-G77 MC9 | 13 | 1334 x 750, 16:9 | |
| Retroid Pocket 5 | Snapdragon 865, Adreno 650 | 13 | 1920 x 1080 AMOLED | the Adreno driver |
| Retroid Pocket Classic, Mini V2, Flip 2 | Snapdragon G1 Gen 2 (Adreno A12 per Retro Catalog) | 14 | Classic: 1240 x 1080, 31:27 | narrower than 4:3 |
| Retroid Pocket 6 | Snapdragon 8 Gen 2, Adreno 740 | 13 | 1920 x 1080, 120 Hz | |

The first Retroid Pocket (Mali-400, Android 6, no Vulkan) is out of reach and not supported.

What this decides:

- **Classic and Balanced must run at full speed on a Mali-G52 MC2** at 640 x
  480, which is exactly twice the game's 320 x 240. Enhanced is optional there.
- **Render passes are required** (step B1): a Mali-G52 on Android
  11 is the kind of Vulkan 1.1 driver that most likely lacks dynamic rendering.
  `vulkaninfo` on a 2S or 3+ confirms it; the plan does not wait on that.
- **Physical controls come first.** A Retroid's built-in pad reaches SDL as a
  gamepad, and that is the main way to play on one. Touch controls show only
  when no gamepad is present, so a handheld never shows them. The controls'
  bindings, analog tuning and pad layout apply to the built-in pad as they do on
  the desktop.
- **Aspect ratios from 31:27 to 20:9.** 4:3 is the game's own; 31:27 is
  narrower and letterboxes top and bottom; 16:9 and 20:9 use the widescreen
  columns. The widescreen code, the HUD at the edges and the menu tiling are
  checked at 31:27 (no negative columns), 4:3, 16:9 and 20:9.
- **Landscape-native handhelds present with no rotation**; pre-rotation (B3)
  matters only on the Pixel.
- **3 GB of RAM on the floor device**: the texture array's 89 MB and the
  rest of the working set are measured there.

## What Android asks of the code we have

Surveyed 2026-10-25 against `port/renderer/vulkan/` and `port/platform/`.

| area | today | on Android |
|---|---|---|
| render passes | `VK_KHR_dynamic_rendering` is **required** (`vulkan_device.cpp`), with `create_renderpass2` and `depth_stencil_resolve` for it on 1.1; five `vkCmdBeginRenderingKHR` sites, and ImGui set up with `UseDynamicRendering` | many Android drivers on Vulkan 1.1 do not have it, so device selection fails there with "none suitable"; the Mali-G52 handhelds are expected among them (B1) |
| surface | `VulkanDevice` creates the `VkSurfaceKHR` once, in `create()`, and keeps it until exit | Android destroys the window when the app goes to the background and gives a new one on return. The surface has to be destroyed and created again with the swapchain, and `VK_ERROR_SURFACE_LOST_KHR` handled |
| present | the target is **blitted** to the swapchain image (`blit_to`), then ImGui draws on top | `vkCmdBlitImage` cannot rotate. Phones are portrait-native, so a landscape game gets `currentTransform` = 90° or 270°, and without pre-rotation the compositor rotates every frame. Presenting with a draw allows both rotation and a final scale |
| swapchain format | B8G8R8A8, R8G8B8A8 as the fallback | R8G8B8A8 is common; the fallback already covers it |
| pacing | FIFO for VSync, mailbox then immediate otherwise | mailbox is rare; displays run at 60/90/120 Hz; the frame rate decides battery and heat |
| 15-bit target | `A1R5G5B5` with blend, else 24-bit (`wanted_color_format` checks the features) | already falls back |
| depth | `D32_SFLOAT`, else `D16_UNORM` | D16 at the game's depth range may z-fight: check. Depth is never stored (`DONT_CARE`), so it can be a transient, lazily allocated attachment on a tiler |
| AO surface buffer | `R32G32B32A32_SFLOAT`, a full-screen pass before the world | 16 bytes a pixel of extra bandwidth: the most expensive Enhanced option on a tiler |
| textures | one 256 x 256 RGBA8 array of 256 layers with 9 mips (`make_pages`): about 89 MB | fine on most phones, heavy on 3 GB ones; measure before changing |
| shaders | GLSL to SPIR-V at build time, no extensions beyond includes, no 64-bit types | nothing seen that mobile lacks; precision (`mediump`) left alone unless profiling says otherwise |
| validation | `KF_VK_VALIDATION`, on in debug | the layer `.so` must be packaged in the debug APK |
| pipeline cache | none | first-launch shader compiles are slow on mobile drivers; keep a `VkPipelineCache` in the user folder |
| overlay font | Windows fonts or `/usr/share/fonts/.../DejaVuSans.ttf` | neither path exists; embed a font |
| logging | `fprintf(stderr)` throughout | stderr goes nowhere on Android; it must reach logcat |
| focus pause | `SDL_EVENT_WINDOW_FOCUS_LOST` pauses the clock and audio (`host.cpp`) | covers backgrounding, but the settings are saved only on exit, and Android kills background apps without one |
| mouse look | `SDL_EVENT_MOUSE_*` turn the view | SDL makes mouse events from touches (`SDL_TOUCH_MOUSEID`) by default; a touch would turn the view and click. Filter them |
| disc import | POSIX `open` on a path; SDL's file dialog | the file dialog gives a `content://` URI, not a path. The extracted data is only 26 MB, so import once into app storage |
| crash reports | a helper process with cpptrace names the stack | no helper process on Android; write the sections and events in-process and leave the stack to the tombstone (`adb bugreport`) |
| Discord | IPC socket | off on Android |
| agent beacon | TCP 27901 on localhost | works unchanged: `adb forward tcp:27901 tcp:27901` lets an agent drive and measure the game on the phone |

## Decisions proposed

- **arm64-v8a, Android 10 (API 29) and later, Vulkan only.** Google requires
  Vulkan 1.1 on 64-bit devices launching with Android 10, and every supported
  Retroid is Android 11 or later, so there is no GLES backend. x86_64 is also
  built, for the emulator only.
- **SDL3's Android project.** `SDLActivity` runs our `main` on its own thread,
  so the decomp's blocking loop and modal waits stay as they are. SDL3 is built
  from source by the Gradle project's CMake. Nothing in the game changes.
- **Sideloaded APK from GitHub releases.** No disc data in the APK; the player
  imports their own image. The Play Store is out of scope.
- **The picture tiers stay the same.** On Android the defaults are chosen by
  measuring on the user's phones (step B6), not guessed.

## Steps

Each step ends with something checked. Anything judged by eye is the user's.

### A. Build and boot (no touch yet)

1. `android/`: SDL3's Gradle template, `applicationId`, landscape lock,
   immersive full screen, `keepScreenOn`, `INTERNET` (the translation download).
   Our CMake builds `libmain.so` with the NDK. Add an `android` nix shell
   (`androidenv`: SDK, NDK, CMake, platform tools) and a `android-arm64`
   preset. glslang runs on the host as it does now.
2. Platform gaps: stderr and stdout to logcat (a pipe and a thread that
   forwards lines); an embedded overlay font; Discord and the crash helper off;
   cpptrace only where it builds.
3. Data: the first run asks for the disc image through SDL's file dialog, reads
   it through `SDL_IOStream` (which opens `content://`), and runs the existing
   import into `SDL_GetPrefPath`. That needs `disc_import_image` to read
   through a stream rather than a path (it already reads the whole image into an
   `AssetTable`). `.chd` goes through libchdr's own reader callbacks. Saves and
   settings already live in the pref path.
4. Arguments: `--skip-intro` and the rest from intent extras
   (`adb shell am start ... -e args "--skip-intro"`), for agents and tests.

Done when: the game boots on the Pixel with a Bluetooth gamepad and on a
Retroid with its built-in pad, draws through Vulkan with validation clean, and
an agent drives it over `adb forward`.

The Retroid needs B1 first.

### B. The renderer on Android

1. **Render passes.** `VkRenderPass` objects replace dynamic rendering
   everywhere (one path; the three extensions go from the required list;
   ImGui's Vulkan backend takes a render pass), since the Mali-G52 handhelds
   need them ("Devices"). Each pass already has its load and store ops spelled out, so this is
   mechanical. `kf_vulkan_tests` runs on both paths on the desktop (llvmpipe and RADV).
2. **Surface lifecycle.** The surface moves out of `VulkanDevice` into the
   swapchain's owner. The queue family is still checked against the first surface;
   a new surface is checked again. On `SDL_EVENT_WILL_ENTER_BACKGROUND`: wait
   idle, destroy the swapchain and surface. On `DID_ENTER_FOREGROUND`: create
   them again. `SURFACE_LOST` takes the same path. Presentation from modal
   waits (`frontend_present`) skips cleanly while there is no surface.
3. **Present by drawing.** `blit_to` becomes a fullscreen draw (the downsample
   pipeline is most of it): it letterboxes, scales nearest at whole multiples and
   smoothed otherwise as now, and rotates by `currentTransform`. ImGui and the
   touch controls draw into a window-orientation image first, so that one
   rotated draw carries them too. Pre-rotation is measured (frame time and
   compositor load with and without it) before it is kept; without it the
   swapchain uses `IDENTITY` and the compositor rotates.
4. **Pacing and refresh.** FIFO with three images. The display's refresh rate
   is chosen with `ANativeWindow_setFrameRate` (or SDL's display mode). Add a
   frame cap (30/60/display) to the Display options, since on a phone it is
   battery and heat. Android's Frame Pacing library (Swappy) is the fallback if
   FIFO alone stutters; the smoothing work already separates updates from shown frames.
5. **Tiler bandwidth.** Depth as a transient attachment
   (`TRANSIENT_ATTACHMENT` usage, `LAZILY_ALLOCATED` memory where offered). The
   AO surface buffer in a smaller format (half floats, or packed normal and depth).
   No pass that reads back the attachment it is drawing to.
6. **Defaults by measurement.** On each test phone, per tier and resolution
   scale: frame time (the frame profiler is not carried yet, so GPU timestamps
   in the backend), temperature after 20 minutes, battery drawn. The Android
   defaults (scale, AO, supersampling off) come from that table.
7. **Pipeline cache** saved to and loaded from the user folder, keyed by
   driver version (`pipelineCacheUUID`).
8. **Tests on the device.** `kf_vulkan_tests` is a plain executable: it builds
   for Android and runs from `/data/local/tmp` over `adb shell`, offscreen, with
   validation on. Run on every test phone and the emulator.
9. **Quirks list.** Driver bugs found on Adreno, Mali and others go in this file
   with the device and driver version, and are worked around by device
   feature, not by vendor name, where possible.

Done when: Classic and Balanced play at full speed on the Mali-G52 floor, and
Enhanced on the devices that can carry it, through backgrounding (and rotation
on the Pixel), judged by the user in play.

### C. Touch controls

The interface first, agreed before the implementation (`code-style.md`).

- **`libverdite/include/verdite/touch.h`** (game-agnostic, unit-tested): a
  layout is a table of controls (stick, button, look area, D-pad), placed in
  units of the safe area. It tracks fingers to controls (each finger keeps the
  control it landed on) and gives back a stick vector, a set of pressed
  buttons, and a look delta in pixels. It does no drawing and has no SDL in it.
- **`port/platform/touch.cpp`**: KF1's layouts as tables mapping controls to
  `Action`. A new `InputDevice::touch` feeds `input_button` and
  `input_motion`. The stick goes into the analog path beside the gamepad's
  (`host_gamepad_sticks` becomes the sum of the active sources), so curves,
  deadzones, twin and tank apply as they do for a pad.
- **`port/ext/touch_overlay.cpp`**: draws the controls on the overlay's
  foreground draw list, translucent, in window pixels.

KF1's layouts, chosen by `kf/platform/screen.h` as Discord already chooses:

| screen | controls |
|---|---|
| walking | a floating move stick on the left half (it starts where the thumb lands); drag on the right half to look (the mouse-look path, its own sensitivity); Attack (large), Magic, Interact and Run at the right thumb; Inventory and Pause at the top; a gear for the settings overlay |
| menus, shops, save screen | a D-pad, Confirm and Back. The menus are pad menus; tapping items directly is not added |
| messages, cutscenes | a tap anywhere is Confirm |

- The Android back button is `pause_or_back`. A long press opens the overlay.
- The controls show only with no gamepad connected (so never on a Retroid),
  hide when a gamepad or keyboard is used, and come back on a touch.
- Options under Controls > Touch: opacity, size, left-handed (mirrored), a
  short vibration on press (if SDL's haptic opens the phone's vibrator).
  Moving buttons around in a layout editor comes later, if wanted.
- The overlay (ImGui) on touch: SDL's touch-to-mouse events go to ImGui and
  never to the game; the scale comes from `SDL_GetWindowDisplayScale`; the
  on-screen keyboard opens with `SDL_StartTextInput` for text fields.
- Controls and HUD keep clear of the notch through `SDL_GetWindowSafeArea`.
  Phones are 19.5:9 to 21:9, wider than the 16:9 the widescreen work was probed at.
  The cull, HUD-at-edges and menu tiling need a check at 20:9.

Done when: floor 1 is played from the start to the first save point on
touch alone on the Pixel, judged by the user. `touch.h`'s finger tracking has unit tests
(two fingers on one control, a finger sliding off a button, lift order).

### D. Life on a phone

- Save the settings on `WILL_ENTER_BACKGROUND`, not only on exit.
- Audio pauses with focus (already) and with audio focus loss (a call).
- Process death in the background loses the game since the last save point.
  See question 3.
- Crash reports in-process: the sections, the last 64 events and the save
  copies are written from the signal handler's safe subset, and the stack is left
  to the tombstone.

### E. Release

- A signed release APK built by CI (the key in the repository's secrets), on the
  GitHub release beside the AppImage and the Windows build.
- `scripts/check.sh` builds the `android-arm64` preset when the Android shell
  is present. The sanitizer runs stay on the desktop, where the same game and
  port code is covered. HWASan on a device is later, if a bug calls for it.
- `README.md`: the install and import steps.

## Order

A1-A2, then B1 and B2, so that the game boots on both kinds of device and
survives backgrounding. Then A3-A4, B3-B5, the touch interface agreed and C,
then B6-B9, D and E. Gyro aim (SDL reads the phone's own gyroscope) is cheap
once C exists and is in the inventory already.

## Decisions

Decided 2026-10-25, with the plan approved:

1. Render passes everywhere (B1), as the devices require.
2. Android 10, arm64, Vulkan only.
3. Process death loses play since the last save point, as the original would:
   no suspend file. Settings are saved on backgrounding (D).
4. Test devices: the user's Pixel 8 Pro. **No Retroid is at hand**, so the
   Mali-G52 floor is covered indirectly: the render-pass path is the only path,
   so `kf_vulkan_tests` on llvmpipe and RADV exercise it; the backend asks for
   nothing beyond Vulkan 1.1 core and what it checks for; the Android emulator
   runs the arm64-less x86_64 build; and the Pixel runs at a Mali-G52-like
   load with the resolution scale and frame cap set to the 2S's (640 x 480).
   A Mali-G52 driver bug can still get through; a Retroid user's report goes in
   the quirks list (B9).
