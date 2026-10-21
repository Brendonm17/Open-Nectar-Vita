# Open Nectar 1.0.7

This release is about stability. Both games were audited for the kind of
code that worked by luck on the GameCube and crashes or misbehaves on a PC
or phone, and Pikmin 2 gets two long-standing audio fixes. Saves, settings
and texture packs carry over.

**Updating:** press **Update** in your launcher. On Android, install the new
APK over the old one.

## Pikmin 2: audio

* **Level music no longer plays over in-game cutscenes** (#135). Discovering
  a treasure, bringing one to the ship, finding a new Pikmin type or drinking
  nectar for the first time now silence the area music as on GameCube.
  Music fade-outs fade again instead of cutting off.
* **Missing sound effects in Stereo and Mono are back** (#135). Plucking
  grass, the ship in caves and the save chime, among others, were only
  heard with the sound set to Surround.
* Sound effects follow their volume and left/right position while they
  play, instead of keeping the values they started with.

## Pikmin 2: crashes and freezes

* **Fixed a crash at the end of the day** (#142).
* **Fixed a freeze when changing areas on Windows** (entering a cave,
  starting Story).
* Fixed crashes while generating cave floors, in some cutscenes, in the
  title, options, file copy and extras menus, in branching dialogues, with
  the Waterwraith and with some sound effects.
* Achievement toasts and keyboard input no longer use memory that is freed
  when the game changes area.

* **Fixed a crash after pressing Begin on Android** and a crash when leaving
  the Piklopedia.
* **Fixed the "memory card" crash when entering a cave** and other rare
  crashes and freezes caused by the game's threads running at the same time.
  Pikmin 2 now schedules its threads like the GameCube did: one at a time,
  by priority, so code written for the console no longer races on PC and
  Android.

## Pikmin 2: performance

* **Piklopedia and Treasure Hoard run at full speed on Android** and their
  3D models load quickly. In widescreen their side bars are now black
  instead of copying the edge colour, which stalled phone GPUs every frame.
* **Faster loading screens** when entering an area, a cave floor or a
  2-Player Battle / Challenge stage.
* Sound mixing runs alongside the game, as the GameCube's DSP did, which
  frees frame time on phones without crackling.

## Pikmin 1: crashes

* Fixed a crash when the collision search gave up, and another with more
  objects on the field than the game expected (high Pikmin limits).
* Fixed crashes and errors with the Burrowing Snagret, the damage effect,
  menus and some debug messages.
* Achievement, versus and captain portrait icons no longer show corrupted
  textures after changing area.

## Both games

* **Better crash reports.** Reports now say why the game stopped (for
  example which check failed and where), Android reports include the call
  stack, and on Windows a failed check opens the crash report instead of
  closing the game silently.
* **Freeze reports (Pikmin 2).** If the game stops responding for 30
  seconds, a report is written and the launcher offers to send it.
* **Divisions by zero no longer crash on Windows and Linux.** The GameCube
  and Android carry on; PC now does too.
* The decompiled code is built with safer compiler settings, which removes
  a whole family of crashes that only happened with optimised builds.

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.7.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
