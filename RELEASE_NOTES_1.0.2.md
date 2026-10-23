# Open Nectar 1.0.2

Pikmin 2 sounds much more complete: many short sound effects that were
missing or barely audible now play. This release also fixes a crash on Linux
when starting Story mode, two Windows launcher problems and several Pikmin 1
bugs. Saves, settings and texture packs carry over.

**Updating:** press **Update** in your launcher. On Android, install the new
APK over the old one.

## Pikmin 2: sound

* **Missing sound effects are back** (#124). Many effects started at
  volume zero and took about a fifth of a second to reach their real level.
  Short sounds (Pikmin voices, footsteps, hits, enemy noises, ship sounds,
  menu clicks) ended before they became audible. They now reach their level
  at once, as on the console.
* **Sound timing matches the console.** The game updates its sounds seven
  times per audio frame. Before, all seven updates came after the frame was
  mixed. Note starts, volume changes and pitch changes now land at the right
  moment, and very short notes are no longer swallowed.
* **Echo and reverb.** The game's own echo effects now play, as on the
  console. Before they were missing entirely.
* With the **Surround** sound option, sounds behind the camera no longer
  go almost silent.

## Fixes

* **Pikmin 2 no longer crashes on Linux when starting Story mode** (#123).
  This happened with the AppImage and the tar package. Development builds
  were not affected.
* **Pikmin 1 can be installed with a European (PAL) disc after Pikmin 2**
  (#126). The installer said the package was missing `nectar-pal.exe`.
* **The launcher shows the Pikmin 2 settings on Windows** (#127). It used to
  say "Could not read the game's settings".
* **HD textures from a zip you already have** (#128). If the download fails,
  for example when Google Drive reaches its daily limit, press
  **Use a .zip I have** and pick the pack you downloaded before. The same
  option is still under Settings > Texture packs > Install a pack from a .zip.
* **Pikmin 1, Blues Only In Water:** a red or yellow Pikmin thrown into water
  drowns as in the original game, so you can still whistle it out. It no
  longer disappears and turns up idle somewhere far away (#125). Pikmin that
  walk into water are still pushed back out.
* **Pikmin 1, Lock-On:** the cursor goes back to its normal distance after
  you release a target, however you release it (#122).
* **Pikmin 1, carrying:** Pikmin that land next to a ship part or pellet walk
  around it to find a free spot. They no longer give up and stand idle,
  which happened at high frame rates (#97).

## For developers

* `PIKMIN_AUDIO_DROPS=1` prints a Pikmin 2 audio report every 5 seconds. It
  shows sounds lost and why, voices that are cut short or inaudible, levels
  per source (effects, music, stream) and clipping. `PIKMIN_FX_GAIN` scales
  the echo (0 turns it off).
* `pc_tev_shader_test` passes again (#113).
* Online co-op (still off in releases): guest input no longer stays ahead
  of the host for a whole session (#95).

## Downloads

| File | System |
|---|---|
| `nectar-windows.zip` | Windows 10 and 11, 64 bit |
| `nectar-linux.tar.gz` | Linux, 64 bit |
| `Open_Nectar-x86_64.AppImage` | Linux, 64 bit, single file |
| `open_nectar_1.0.2.apk` | Android |

You need your own Pikmin and Pikmin 2 discs (ISO, GCM, GCZ or RVZ). No game
data is included or downloaded.
