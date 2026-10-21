# Open Nectar 1.0.6

This release fixes a long list of Pikmin 2 bugs reported by players, adds a
crash reporter to both games and tidies up the Pikmin 1 settings menu. Saves,
settings and texture packs carry over.

**Updating:** press **Update** in your launcher. On Android, install the new
APK over the old one.

## Crash reports

* **If a game crashes, the launcher opens with a crash report.** One button
  copies the report and the game log to your clipboard and opens a new GitHub
  issue, so you only have to paste it and send it. Your user name is removed
  from file paths. On Android the report is offered the next time you open
  the app.
* **Pikmin 1 now writes `pikmin.log`** when started from the launcher, like
  Pikmin 2 does with `pikmin2.log`.

## Pikmin 2: fixes

* **Fixed a crash on Android when pressing Begin** on the title screen.
* **Fixed wrong ship textures after leaving a cave.**
* **The radar map in the pause menu is no longer empty.**
* **The ship icon animates again** in dialogues and Explorer Kit messages,
  and Explorer Kit treasures show their correct message.
* **Cursor rings no longer flicker** at 60 FPS (#136).
* **Enemy animation sounds no longer replay** at the end of an animation.
* **60 FPS audit:** timers, fades and effects that ran twice as fast at
  60 FPS now keep the original speed.

## Pikmin 1: controls and settings

* **D-pad Down lies Olimar down again on a controller** (#138). Swarm to
  cursor is now its own controller binding in Controls > Bindings, with or
  without Free Camera.
* **Tidier settings menu.** Keyboard and controller bindings are now at the
  top of Controls. Options that depend on another one (gyro sensitivity,
  mouse sensitivity, colour sliders, Charge) are hidden until that option is
  turned on, instead of showing greyed out.
* **The Language option was removed** from the port's menu: choose the
  language in the game's own options, as on GameCube.

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.6.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
