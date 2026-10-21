# Open Nectar 1.0.5

This release restores short sound effects and Pikmin voices that could be cut
off in Pikmin 2. It also improves Linux controller compatibility and fixes the
Pikmin 1 logo in packaged builds. Saves, settings and texture packs carry over.

**Updating:** press **Update** in your launcher. On Android, install the new
APK over the old one.

## Pikmin 2: audio

* **Short sound effects and Pikmin voices play correctly again.** The native
  audio backend could discard short, lower-priority sounds while filling its
  audio queue, particularly when several sounds played close together.
* **More reliable sound data handling.** The ARAM and DSP compatibility code
  now preserves the data and timing expected by the original audio engine.

## Pikmin 1: Linux compatibility

* **Improved controller support on Linux.** Packaged builds now include the
  required input compatibility path for controllers that were not detected
  correctly before.
* **The Pikmin 1 logo is included correctly** in packaged builds.

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.5.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
