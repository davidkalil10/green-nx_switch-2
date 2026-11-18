# green-nx

[![Ko-fi](https://img.shields.io/badge/Ko--fi-Support%20this%20project-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/davidkalil10)

A standalone, open-source **Xbox Cloud Gaming (xCloud) client for the Nintendo
Switch and Nintendo Switch 2** (homebrew). Sign in with your Microsoft account
and stream your Game Pass library natively — WebRTC connection, hardware H.264
decoding and GPU rendering are all done on the console, with no companion apps
or PCs involved.

**Fully working:** sign-in, game library with box art and search, and smooth
720p / 1080p / 1080p HQ streaming with audio and full controller input.

> **Nintendo Switch 2 support** added by **David Kalil Braga** ([@davidkalil10](https://github.com/davidkalil10)), compatible with **hbmenu** (`ounce-hbmenu`) and **kaiHEN**, using [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (`libounce` + `OunceVK` + `GSV1/viddec`).
> Special thanks to **rmrf404** and the original **[rmrf404/green-nx](https://github.com/rmrf404/green-nx)** project (and its contributors) for creating and maintaining the original Nintendo Switch client that made this Switch 2 port possible.

## Features

- Microsoft **device-code sign-in** (no password typed on console); tokens
  cached on SD
- **Game library** with box-art covers, quality settings, and search that
  covers the **whole xCloud catalog** — titles you cannot stream yet are
  flagged with what they need (Game Pass, ad-supported), and a game that is
  not on xCloud at all says so instead of "nothing found"
- **Native WebRTC streaming**: ICE (Teredo), DTLS-SRTP, SCTP data channels,
  RTP video/audio, NACK/PLI recovery — implemented on-console
- **Hardware H.264 decoding** (`NVDEC` via FFmpeg's `NVTEGRA` hwaccel on Switch 1; native `GSV1` / `viddec` on Switch 2) with **GPU rendering** (`deko3d` zero-copy on Switch 1; `OunceVK` OpenGL ES 3.0 with luma sharpening and debug HUD on Switch 2)
- Opus audio, 60 fps video, three quality tiers (720p / 1080p / 1080p HQ)
- **Ad-supported streaming** for free-to-play titles: no subscription needed,
  streamed from Microsoft's ad-supported offering (sessions are capped by the
  server, and the console tells you before you start)

## Requirements

- **Nintendo Switch:** running the [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware
- **Nintendo Switch 2:** compatible with **hbmenu** (`ounce-hbmenu`) and **[kaiHEN](https://github.com/ouncebrew/kaihen)** (use `green-nx-switch2.nro`)
- An **Xbox Game Pass** subscription for the subscription catalog — every tier
  (Essential, Premium, Ultimate) includes Cloud Gaming; Ultimate streams the
  largest catalog at the highest quality. Games you already own stream without
  one, and so do free-to-play titles offered with ads
- A 5 GHz Wi-Fi connection is recommended

## Install

1. Copy `green-nx.nro` (Switch 1) or `green-nx-switch2.nro` (Switch 2) to `sdmc:/switch/` on your SD card (or send via Netloader / `nxlink`).
2. On Switch 1, launch **in title mode** — hold **R** while starting an installed game.
   (Applet mode does not have enough memory for hardware video decoding.)
   On Switch 2, launch `green-nx-switch2.nro` from `ounce-hbmenu`.
3. Sign in once with the device code shown on screen (`microsoft.com/link`).
   Tokens and the game list are cached in `sdmc:/switch/green-nx/`.

### Controls

| Context | Buttons |
| --- | --- |
| Library | Left stick / d-pad move · **A** play · **Y** search · **X** refresh · **ZL** settings · **-** sign out · **+** exit |
| Settings | Stream quality (720p / 1080p / 1080p HQ) · button layout (positional / labels) |
| In stream | Xbox controls mapped from the Switch pad · hold **-** + **+** to quit |

## Build

No local toolchain needed — everything builds inside the
`devkitpro/devkita64` Docker image:

```sh
# 1. Build the WebRTC dependencies once (libpeer, libsrtp, usrsctp, mbedTLS)
bash deps/build-switch.sh

# 2. Build the app
docker run --rm -v "$PWD":/src -w /src devkitpro/devkita64 make
```

The result is `green-nx.nro`. A small desktop harness
(`make -f Makefile.pc`) builds the core (auth/catalog/signaling) for
development on a PC:

```sh
./build/pc/greennx login            # device-code sign-in
./build/pc/greennx games            # the playable library, with names
./build/pc/greennx search cyberpunk # the whole catalog, with availability
./build/pc/greennx stream-test FORTNITE --ads   # ad-supported offering
```

### Nintendo Switch 2 (`green-nx-switch2.nro`)

Built with devkitA64 + [ounce-devkit](https://github.com/ouncebrew/ounce-devkit) (`libounce` + `OunceVK`):

```sh
# 1. Build the Switch 2 WebRTC dependencies once (libpeer, libsrtp, usrsctp, mbedTLS)
bash deps/build-switch2.sh

# 2. Build the Switch 2 .nro inside an ounce-devkit container
docker run --rm -v "$PWD":/src -w /src ounce-devkit make -f Makefile.switch2

# 3. Verify Switch 2 OUN0 startup header, ASET assets and link map
python3 tools/verify_switch2_package.py green-nx-switch2.nro --link-map build-switch2/green-nx-switch2.map
```

### Layout

```
src/core/            auth, catalog, HTTP, xCloud session + protocol
src/switch/          UI (SDL2), covers, input
src/switch/stream/   streaming engine: WebRTC glue, RTP jitter buffer,
                     NVDEC/deko3d (Switch 1) & viddec/OunceVK GLES3 (Switch 2),
                     Opus audio
shaders/             deko3d video shaders (compiled by uam during Switch 1 make)
deps/                build scripts (build-switch.sh / build-switch2.sh) + patches
tools/               Switch 2 NRO & link-map verifier (verify_switch2_package.py)
```

## Credits & Third-party software

- **Original project:** [rmrf404/green-nx](https://github.com/rmrf404/green-nx) by **rmrf404** and contributors — thank you for building and open-sourcing `green-nx`!
- **Nintendo Switch 2 port:** **David Kalil Braga** ([@davidkalil10](https://github.com/davidkalil10)) — Switch 2 `libounce` (`GSV1`/`viddec`) hardware H.264 decoder, `OunceVK` OpenGL ES 3.0 renderer, `Makefile.switch2`, dependency builder and CI verification.
- **Switch 2 toolchain & libraries:** [ounce-devkit](https://github.com/ouncebrew/ounce-devkit), [libounce](https://github.com/ouncebrew/libounce), [OunceVK](https://github.com/ouncebrew/ouncevk), and [kaiHEN](https://github.com/ouncebrew/kaihen).

green-nx builds on these open-source projects:

| Project | License | Use |
| --- | --- | --- |
| [libpeer](https://github.com/sepfy/libpeer) (patched) | MIT | WebRTC: ICE / DTLS-SRTP / SCTP |
| [libsrtp](https://github.com/cisco/libsrtp) | BSD-3 | SRTP encryption |
| [usrsctp](https://github.com/sctplab/usrsctp) | BSD-3 | SCTP data channels |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | Apache-2.0 | TLS / DTLS crypto |
| [FFmpeg](https://ffmpeg.org) (devkitPro build with [averne's](https://github.com/averne/FFmpeg) NVTEGRA hwaccel) | LGPL-2.1+ | H.264 hardware decoding |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib | GPU rendering |
| [SDL2](https://libsdl.org), SDL2_ttf, SDL2_image | zlib | UI rendering, input |
| [Opus](https://opus-codec.org) | BSD-3 | Audio decoding |
| [libcurl](https://curl.se) | curl | HTTPS |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT | JSON |
| [devkitPro / libnx](https://devkitpro.org) | various | Switch toolchain and OS interface |

The patches applied to libpeer (keyframe requests, NACK, receiver reports,
REMB, data-channel fixes, Switch port) are in `deps/patches/`.

## Disclaimer

green-nx is a **fun, experimental, non-commercial** hobby project, provided
as-is for personal use. It is **not affiliated with, endorsed by, or supported
by Microsoft or Nintendo** in any way. Xbox, Xbox Cloud Gaming and Game Pass
are trademarks of Microsoft. Nintendo Switch is a trademark of Nintendo. You
need your own valid Game Pass subscription; this project only
implements a client for a service you already pay for.

## License

[GPL-3.0](LICENSE)

## Support

If you find **green-nx (Switch 2 Port)** and our Nintendo Switch 2 homebrew ports useful, consider buying me a coffee to support continued development and maintenance:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/davidkalil10)

👉 **[https://ko-fi.com/davidkalil10](https://ko-fi.com/davidkalil10)**
