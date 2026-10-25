# Open Nectar 1.0.9.1 — Windows antivirus fix

**Windows only.** Linux, macOS and Android stay on
[1.0.9](https://github.com/SSunnKing/Open-Nectar---Pikmin-Native-PC-Mobile-Port/releases/tag/1.0.9).
Download those from there. Nothing changes in the games themselves.

**Updating:** press **Update** in your launcher. Saves, settings and texture
packs carry over.

## Windows Defender flagged the launcher as a trojan

Some players got a "Threat found" warning from Windows Defender for
`nectar-launcher.exe` (`Trojan:Win32/Bearfoos.A!ml`). This was a false
positive. The `!ml` means it came from Defender's machine-learning guess,
not from a real malware match.

The launcher was doing things that look suspicious to an antivirus: it
started `curl.exe` and `tar.exe` in the background to download updates and
texture packs, and the exe had no publisher or version info. This release
changes that:

* Downloads now happen inside the launcher instead of through `curl.exe`.
* Update zips are unpacked inside the launcher instead of through `tar.exe`.
* `nectar.exe`, `nectar-pal.exe` and `nectar-launcher.exe` now have
  publisher and version information (right-click → Properties → Details).

The false positive has also been reported to Microsoft.

**If Defender still blocks it:** open Windows Security → Protection history,
select the detection and choose **Allow on device**. Only do this with files
downloaded from this releases page.
