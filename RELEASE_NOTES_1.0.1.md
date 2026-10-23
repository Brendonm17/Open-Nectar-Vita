# Open Nectar 1.0.1

A quick fix for Pikmin 2 with the **USA disc (GPVE01)**: the game stayed on a
black screen right after starting, on every system. It now boots normally.
Saves, settings and texture packs carry over.

**Updating:** press **Update** in your launcher. On Android, install the new
APK over the old one.

## Fixes

* **Pikmin 2 USA no longer hangs at start.** The frame timing waited for a
  display signal that only exists on the console. All regions now use the
  same timing as the European version, which was already working.
  The same fix covers the European disc in 60 Hz mode.
* With the USA disc, the game first shows the original **health and safety
  warning**: press any button to continue, or it moves on by itself after a
  minute.
* On the USA disc the Randomizer appears on the title screen as **Random**:
  that disc has no "z" letter in its title graphics.

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.1.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
