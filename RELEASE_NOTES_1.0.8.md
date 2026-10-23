# Open Nectar 1.0.8

Fixes for display, controllers, crashes and Pikmin 2 sound reported since
1.0.7. Saves, settings and texture packs carry over.

**Updating:** press **Update** in your launcher. On Android, install the new
APK over the old one.

## Pikmin 1: display (Windows)

* **VSync now really stops screen tearing** (#144). With Frame Sync (VSync)
  On, the 30 FPS and 60 FPS modes only limited the frame rate with a timer
  and never waited for the monitor, so the picture tore. They now wait for
  the screen's refresh like the Monitor FPS mode.
* **Fixed a thin coloured line at the bottom of the screen** (#144) at
  resolutions where the GameCube picture does not scale by a whole number
  (for example 1920×1200). Some screen areas were rounded to one pixel short.

## Both games

* **Exit option in the title menu.** Both games can now be closed to the
  desktop from the main menu, without Alt+F4.
* **Remapped triggers no longer press both L and R** when they were swapped
  or assigned to another action.
* **Android: the controller you press first becomes Player 1.** Some phones
  list a virtual device before the real controller, which then did nothing.

## Pikmin 1: fixes

* **Skipping an Onion discovery scene no longer leaves the Onion without its
  first seed.**
* The launcher checks that the game files needed at start-up are all present
  and offers to repair an incomplete installation, instead of the game
  stopping with an error (#151).

## Pikmin 2: crashes

* **Fixed a crash when breaking the last segment of a gate** (#153).
* **Fixed a crash when changing area with frame interpolation on** (#150).
* Android crash reports can now name where the game stopped even when the
  call stack cannot be read.

## Pikmin 2: sound

* **Fixed silent sound effects** stored in the GameCube's 2-bit audio format.
* **Sound works on Windows devices that only accept 48 kHz**, and comes back
  after a headset or audio device is unplugged instead of staying silent.
* Loud scenes with many effects no longer distort.
* **The Mono sound option is remembered** instead of returning to Stereo on
  every launch.

## Pikmin 2: other fixes

* Fixed stale screen captures and missing cut-out details in some scenes,
  boss music in the Randomizer, spray absorption and some menu wording.

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.8.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
