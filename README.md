# Aurora

Unofficial fork of [Moonlight TV](https://github.com/mariotaku/moonlight-tv) for **LG webOS** (C1–C5 and compatible sets), focused on high-quality streaming on OLED TVs with a remote- and gamepad-friendly UI.

> Rights to the original project belong to [mariotaku/moonlight-tv](https://github.com/mariotaku/moonlight-tv) and the Moonlight community. Provided without warranty.

## Highlights

- **AMOLED layout** — pure black background, dark surfaces, violet accent.
- **3.6K (3584×2016)** recommended on LG C5 (stable quality without native-4K cumulative delay); **4K** when the set handles it.
- **HDR10 (PQ)** over HEVC Main10 (when supported).
- Bitrate slider up to **300 Mbps**; above **250 Mbps** there is usually no visible gain and packet loss becomes more likely as the link becomes less stable.
- **Performance stats overlay**, **full on-screen keyboard**, and **virtual mouse** during streaming.
- **Redesigned settings** — row-based D-pad navigation, fixed combobox/checkbox remote handling, modal Host/Input/Experimental panes.
- **On-screen log overlay** (Experimental) — punktfunk-style live log tail; Magic Remote **Yellow** cycles Live → Frozen → Off.
- **Rooted game mode** (Experimental, rooted TVs) — TV picture/sound and motion/energy pack tuned for streaming; restored on exit.
- **Select server** popup uses home-screen style app tiles instead of a plain list.

## Screenshots

| Home | Settings |
|:---:|:---:|
| ![Home screen](docs/images/home.png) | ![Basic settings](docs/images/settings.png) |

| Performance stats | On-screen keyboard |
|:---:|:---:|
| ![Performance stats](docs/images/performance-stats.png) | ![On-screen keyboard](docs/images/keyboard.png) |

![Streaming with keyboard and stats](docs/images/keyboard-streaming.png)

## Quick start

| Setting | Suggestion |
|---------|------------|
| Resolution | **3.6K** (3584×2016) on LG C5; **4K** if your set is stable at native 4K |
| FPS | 60 or 120 |
| Codec | HEVC (H.265) |
| Bitrate | Start at **120–180 Mbps**; stay at or below **250 Mbps** for a stable link. Higher values rarely help and can increase packet loss. |

## Streaming controls

**Open overlay:** Magic Remote **RED** / **EXIT**, or gamepad **LB + RB + Back + Start** (hold, then release).

| Gesture | Action |
|---------|--------|
| **L1 + R3** (Xbox: LB + RS) | Toggle virtual mouse |
| **R1 + R3** (Xbox: RB + RS) | Toggle on-screen keyboard |
| **Circle / B** while keyboard is open | Close the keyboard |
| **Hold Select/Back 4s** | Toggle pinned performance stats |
| **Y / Triangle** (virtual mouse on) | Open on-screen keyboard |

Aurora handles these shortcuts itself and drops the button presses instead of passing them to the host, so closing the keyboard (or toggling a mode) never also cancels something inside the game.

Full keyboard: stream overlay, Magic Remote **BLUE**, gamepad **R1 + R3**, or **Y** while virtual mouse is active. With the keyboard open: **Y** = Space, **LT** = abc/`&123`, **LB/RB** = Left/Right, **Circle/B** = close. Virtual mouse: stream overlay button or **L1 + R3** (or enable in Settings → Input to start enabled); right stick = cursor, left stick = scroll, LT/RT = mouse buttons.

Details, hotkey layout, and stats field reference: [webOS build guide](docs/BUILD_WEBOS.md).

## Worn sticks (drift)

A DualShock 4 plugged into the TV is read by webOS through the generic HID path: there is no
DualShock driver, so the pad is used without its own centre calibration and without the deadzone
the Linux kernel applies on a PC. Sticks that are already worn then drift slowly on their own,
and a direction held at the end of its travel can blink out for one report — the character
hesitates while the thumb is still pushing.

Enable **Settings → Input → Stick drift correction** (and raise the deadzone until the idle stick
stops moving the game). Aurora then cuts the deadzone *and* stretches what is left over the whole
range, so the stick still reaches full deflection and the edge of the deadzone fades instead of
stepping, and it keeps a held direction when a report collapses back to the centre, releasing it
as soon as the pad confirms the change. While the toggle is on, Aurora also repeats the state of a
controller that is doing something — a stick pushed to the end of its travel sends nothing at all,
so without that a missed controller arrival would leave the direction released. The toggle applies
to the next stream; leave it off to keep the plain deadzone behaviour.

To check whether the pad really is dropping reports on your set, turn on the on-screen log overlay
in **Settings → Experimental**: with drift correction active Aurora logs every report it held back
and every direction it released when a hold expired.

## Install

- [webOS Homebrew Channel](https://github.com/webosbrew/webos-homebrew-channel) — repo: `https://raw.githubusercontent.com/GuiDev1994/aurora-tv/main/repo.json`
- [Device Manager](https://github.com/webosbrew/dev-manager-desktop) — install the latest `.ipk` from [Releases](https://github.com/GuiDev1994/aurora-tv/releases)
- [webOS TV CLI](https://webostv.developer.lge.com/develop/tools/cli-installation) — `ares-install com.aurora.gamestream_*_arm.ipk` ([build guide](docs/BUILD_WEBOS.md))

## Build from source (developers)

Cross-compile Aurora for webOS using Docker. **Prerequisites:** Docker Desktop (Windows/macOS) or Docker Engine (Linux).

**Windows (PowerShell):**

```powershell
.\scripts\webos\build_with_docker.ps1
```

**Linux / macOS (Docker):**

```bash
docker run --rm \
  --dns 8.8.8.8 --dns 1.1.1.1 \
  -v "$(pwd):/build" \
  -v "$(pwd)/scripts/webos/docker_build_inner.sh:/docker_build.sh" \
  -w /build -e CI=1 -e DOCKER_SKIP_SUBMODULES=1 \
  ubuntu:22.04 \
  bash -c "sed 's/\r$//' /docker_build.sh | bash"
```

The `.ipk` is written to `dist/com.aurora.gamestream_<version>_arm.ipk`. To generate a Homebrew manifest locally (optional), install `webosbrew-gen-manifest` once; official [releases](https://github.com/GuiDev1994/aurora-tv/releases) build and publish the manifest via GitHub Actions.

Full build, install, and troubleshooting guide: [docs/BUILD_WEBOS.md](docs/BUILD_WEBOS.md).

## Credits

- Base: [mariotaku/moonlight-tv](https://github.com/mariotaku/moonlight-tv)
- Components: [moonlight-embedded](https://github.com/irtimmer/moonlight-embedded), [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c)

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE) (GPL-3.0-or-later).

Copyright and attribution details are in [COPYRIGHT](COPYRIGHT).

Aurora is a fork of [moonlight-tv](https://github.com/mariotaku/moonlight-tv), which is also licensed under GPL-3.0.
