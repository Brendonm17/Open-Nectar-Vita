# Open Nectar 1.0

**Pikmin 2 runs natively for the first time.** Open Nectar is no longer a
Pikmin port: it is now the home of both GameCube games, built from their
decompilations and running as native code on Windows, Linux and Android, with
no emulator involved. One launcher, one APK, both games, and Pikmin 2 is
playable from start to finish.

Alongside Pikmin 2, both games get a Randomizer, Pikmin 1 gets a VS map
editor, smoother motion on high refresh rate monitors and a long list of
fixes. Saves, settings and texture packs carry over.

**Updating from 0.9.2:** open your installed launcher and press **Update**.
The 0.9.2 launcher only knows how to install Pikmin 1, so when the new one
starts it offers to **Finish the update** and fetches the rest (Pikmin 2 and
the map editor) by itself. On Android, install the new APK over the old one.

## Pikmin 2

A native port of Pikmin 2, built on the
[projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) decompilation,
playable from start to finish. Install it from your own disc, from the same
launcher as Pikmin 1. Both USA and European discs are supported.

* **Smooth at any frame rate.** Gameplay keeps the original timing while
  animations and cutscenes are interpolated to 60, 120 FPS or your monitor's
  refresh rate.
* **Looks like the console.** Real DSP audio levels, the see through helmet
  glass with its reflections, indirect texture effects (heat haze, geysers,
  cave holes), and fades without flicker at high frame rates.
* **Randomizer** mode, for a different run every time.
* **New Game window** with difficulty rules: **Hard** and **Permadeath**.
* **Speedrun mode** with timer, splits and best times, as in Pikmin 1.
* **Captain selector:** pick who you play as, with tint, HUD and voices to
  match, from a 3D selector.
* **The F1 menu,** redrawn with Pikmin 2's own assets, with button icons in
  its help texts. Same layout as Pikmin 1.
* **Optional gameplay mods** shared with Pikmin 1: swarm that gathers the
  squad at the cursor with Wii style arrows, Better Pathfinding, Blues Only In
  Water, Lock-On and Charge, Free Camera zoom, Eternal Night with fireflies
  and the moon. All off by default.
* **Texture packs**, including a one click HD pack download from the
  launcher.
* **Android:** in the same APK as Pikmin 1, with touch controls laid out like
  Pikmin 1's, touch menus and touch button icons in the game's texts.

## Pikmin 1

### New modes and options

* **Randomizer:** ship parts, enemies and bosses shuffled, from a title menu
  entry, with share codes so friends can play the same seed.
* **Speedrun:** categories, LiveSplit support and a day selector.
* **New Game window:** difficulty and game mode in one place.
* **Lock-On and Charge** for every captain, in local co-op too, with modes
  and their own ring.
* **Swarm at the cursor** with Wii style arrows.
* **Better Pathfinding** (thrown Pikmin with nothing to do return to the
  squad, stuck carriers find a new route) and **Blues Only In Water**.
* **Eternal Night** with fireflies and the moon.
* **Captain selector** with 3D cards. Pikmin captains, Puffmin, and with
  Pikmin 2 installed, Louie, the President, White and Purple Pikmin and
  Bulbmin taken from your own Pikmin 2 disc.
* F1 menu reorganised, with keyboard navigation in the menus.

### Map editor

* **Nectar Map Maker,** an editor for VS arenas, opened from the **Map
  editor** button in the launcher.

### Graphics and performance

* **Visual interpolation at your monitor's refresh rate,** on top of the 30,
  60 and 120 FPS modes.
* Per frame counters scaled for 60 and 120 FPS (drowning, tripping and
  others), and seeds that plant in the ground at high frame rates.
* Olimar's helmet glass is transparent again. Fireflies no longer show as
  white squares in the dawn mist.

## Launcher

* **One home for both games:** a single **Update** for everything, and
  **Install**, **HD textures** and **Map editor** under the covers.
* **HD textures:** downloads and installs the community HD pack for each game
  (4K or 1080p on PC, the mobile pack on Android), with progress, a free space
  check and a **Download again** option. Packs are downloaded on demand; they
  are not part of the release.
* **Finishes old updates:** an install updated by an older launcher is
  completed automatically.
* Large downloads no longer give up after 30 seconds; they only stop if the
  connection stalls.
* Desktop shortcut, and the window stays on screen when it is resized
  (thanks knakamura13).

## macOS

* Both games build and run on macOS from source. There is no ready made
  macOS package: see **Build from source** in the
  README. Faster vertex uploads on Apple's OpenGL (thanks knakamura13).

## Fixes

* Windows releases no longer require a recent CPU; this fixes the crash at
  start on older processors (#41, #77).
* The last day ends at sunset with Infinite Day on.
* Whistle Pluck works on Yellow and Blue Pikmin.
* With no colour chosen, throws take the nearest Pikmin, as on the console
  (thanks knakamura13).
* A Yellow placing a bomb warns the others again, bomb carriers included, as
  on the console (thanks knakamura13).
* Skipping a day's arrival leaves the Onions landed and the captain in
  control.
* Pikmin 2: the x03 Onion appears only once, and the day results can't be
  skipped by accident.

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
