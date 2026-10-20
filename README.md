# NVLOW

**Shrink NVIDIA recordings without losing quality. One small exe (~0.5 MB), nothing to install.**

NVIDIA ShadowPlay / NVIDIA App records at very high bitrates (a 2-minute 1920×1200 clip is often 400–450 MB).
NVLOW re-encodes those clips on your GPU. A typical result is **60–70 % smaller** with the picture visually
identical, and much smaller with the lighter presets. It keeps the original resolution, frame rate, and all audio
tracks (copied bit-for-bit).

## Features

- **Finds your NVIDIA recordings automatically.** It reads the capture path from NVIDIA's settings and falls back
  to `Videos\NVIDIA`, then lists every game folder (e.g. `Counter-strike 2`) with a game filter.
- **Any other video too.** Use *Add videos…* or drag and drop files or folders (mp4, mov, mkv, avi, wmv, ts, …).
- **GPU-accelerated through Windows Media Foundation.** No FFmpeg and no other DLLs. It decodes, scales and
  encodes on the GPU: AV1 (RTX 40/50), HEVC and H.264 on NVIDIA. AMD and Intel GPUs work as well, and it falls
  back to the CPU when needed.
- **Always picks a format the PC can play.** It checks for the Windows AV1 and HEVC video add-ons and
  auto-selects the best format that will play in Windows' own players.
- **Quality presets:** Visually lossless, High (default), Balanced, Smallest. Optional downscale to 1440p, 1080p
  or 720p (never upscales).
- **Safe by design:**
  - Writes to a `.part` file and renames it only on success.
  - Never touches originals unless you opt in to the Recycle Bin.
  - Discards the result if it would not be smaller.
  - Keeps the original timestamps so your gallery stays in recording order.
- Per-file and overall progress, speed, ETA, taskbar progress, and cancel at any time.

Compressed files are saved as `<name>_nvlow.mp4` in `Videos\NVLOW\<Game>\` (configurable), or next to the original.

## Sharing

Send `dist\NVLOW.exe`; it is the whole app. It runs on Windows 10/11 64-bit.
(On Windows "N" editions, install the Media Feature Pack first.)

Because the exe is not code-signed, Windows SmartScreen may show "Windows protected your PC".
Click **More info → Run anyway**.

## Building

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT   # GCC + CMake + Ninja
.\build.ps1                                       # -> dist\NVLOW.exe
```

MSVC also works: `cmake -S . -B build -G "Visual Studio 17 2022" && cmake --build build --config Release`.

## Building the installer

NVLOW ships with an [Inno Setup](https://jrsoftware.org/isinfo.php) script, `installer/NVLOW.iss`.

```powershell
winget install JRSoftware.InnoSetup     # one time
.\build.ps1 -Installer                  # -> dist\NVLOW-Setup-<version>.exe
```

What the installer does:

- Installs per user by default, so no admin prompt (`%LOCALAPPDATA%\Programs\NVLOW`); it can also install
  for all users.
- Adds a Start Menu shortcut, an optional Desktop shortcut, and an entry in *Settings → Apps* with an uninstaller.
- Closes a running NVLOW before upgrading and offers to launch it when finished. English and French.
- Uninstalling keeps the user's settings and compressed videos.

**Releasing a new version:**
1. Change `VERSION x.y.z` in `CMakeLists.txt`. The installer reads it from there.
2. Change the version in `res/nvlow.rc` (FILEVERSION, PRODUCTVERSION and the two version strings).
3. Run `.\build.ps1 -Installer`.
4. Share `dist\NVLOW-Setup-x.y.z.exe`. Running it over an older version upgrades in place, because `AppId` in the
   `.iss` never changes.

Silent install (IT / scripts): `NVLOW-Setup-x.y.z.exe /VERYSILENT /CURRENTUSER /TASKS=desktopicon`

## Publishing an update (auto-update)

NVLOW checks this repository's **GitHub Releases** at startup. When the latest release is newer than the running
version, users see a bar: *"NVLOW x.y.z is available — Update now"*. One click:

1. downloads `NVLOW-Setup-x.y.z.exe` from the release,
2. checks its size and SHA-256 against GitHub's checksum,
3. runs it silently,
4. reopens NVLOW on the new version. Settings are kept.

Portable copies (not installed) get a **Download** button that opens the release page. Anyone can also use
**Check for updates…** in the window menu (click the app icon in the title bar).

**To publish version x.y.z:**
1. Bump the version in `CMakeLists.txt` and `res/nvlow.rc`, then commit.
2. Run `.\build.ps1 -Installer`.
3. On GitHub, open **Releases → Draft a new release**:
   - **Tag:** `vx.y.z` (for example `v1.2.0`). It must be higher than the previous one.
   - **Title:** `NVLOW x.y.z`
   - **Notes:** describe what changed. Users see this under "What's new".
   - **Attach** `dist\NVLOW-Setup-x.y.z.exe`. The name must start with `NVLOW-Setup`.
   - Leave "pre-release" unticked, then click **Publish release**.

   With the GitHub CLI, this step is one command:
   `gh release create vx.y.z dist\NVLOW-Setup-x.y.z.exe --title "NVLOW x.y.z" --notes "What changed..."`

Every installed copy offers the update the next time it starts. The repository is set in `CMakeLists.txt`
(`NVLOW_GITHUB_REPO`). It must be **public**, so apps can read releases without logging in.

## Performance setting

| Level | Behaviour | 2-min 1200p60 clip (RTX 5070) |
|-------|-----------|------------------------------|
| Low | Speed capped at 2× realtime, NVLOW runs at below-normal priority: quiet fans, games and other apps stay smooth | ~60 s |
| Medium (default) | Balanced encoder preset | ~16 s |
| High | Fastest encoder preset, full GPU power | ~8 s |
| Max quality | Slowest preset, best detail | ~45 s |

File size is the same at every level. Measured quality: VMAF 95.0 (High) to 95.6 (Max quality), a difference you can't see.

## Project layout

```
src/
  main.cpp           entry point, single-instance guard, Media Foundation startup
  MainWindow.*       Win32 UI: library list, settings, queue, progress
  VideoScanner.*     NVIDIA folder detection and library scan
  MediaProbe.*       video info via Media Foundation, background probe queue
  Encoder.*          GPU transcoder (Source Reader -> D3D11 -> encoder MFT -> MP4 Sink Writer)
  Settings.*         %APPDATA%\NVLOW\settings.ini
  Utils.*            formatting, paths, Store add-on detection
res/                 icon, manifest (Common Controls v6, PerMonitorV2), version info
tools/make_icon.ps1  regenerates res/nvlow.ico
```

## Quality calibration

The bitrate scales with resolution and frame rate from these 1920×1200 @ 60 fps reference values, measured
with VMAF on CS2 gameplay (27 Mbps H.264 source, RTX 5070 Laptop GPU). VMAF ≥ 95 is generally
indistinguishable from the source at normal viewing.

| Preset            | AV1      | HEVC    | H.264   | Target VMAF |
|-------------------|---------:|--------:|--------:|------------:|
| Visually lossless | 12 Mbps  | 15 Mbps | 16 Mbps | ~97.5 |
| High (default)    | 8.5 Mbps | 10 Mbps | 12 Mbps | ~95.5 |
| Balanced          | 6 Mbps   | 7.5 Mbps| 9.5 Mbps| ~93 |
| Smallest          | 4.5 Mbps | 5.5 Mbps| 7 Mbps  | ~90 |

Measured: HEVC at 10 Mbps gives 387 MB → 157 MB with VMAF 95.6; AV1 at 9 Mbps gives 133 MB with VMAF 96.0.
