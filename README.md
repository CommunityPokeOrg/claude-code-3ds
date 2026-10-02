# claude-code-3ds

Nintendo 3DS homebrew client for Claude — a terminal-style chat UI that
talks to the [Anthropic Messages API](https://docs.anthropic.com/en/api/messages)
from a hacked 3DS, built with devkitPro/libctru. Produces a `.3dsx` for the
Homebrew Launcher and a `.cia` installable via FBI.

This is a **first pass**: a working single-shot-per-turn chat client with the
plumbing in place (TLS, conversation state, scrollback, software keyboard,
SD-card settings). It is not the full Claude Code agentic tool loop — see
[Implemented vs. stubbed](#implemented-vs-stubbed).

Ported from [claude-code-switch](https://github.com/CommunityPokeOrg/claude-code-switch).

![icon](icon.png)

## Implemented vs. stubbed

### Implemented

- **Dual-screen terminal UI**: top screen shows a status bar plus the chat
  transcript (50x30 text grid, color-coded user / assistant / system / error);
  bottom screen shows the compose box, the stored draft, and a hint bar.
  Settings render on the bottom screen while the transcript stays on top.
- **Scrollback**: 512-line wrapped transcript buffer; D-pad/circle pad scrolls
  a line at a time, `L`/`R` page up/down, auto-pins to bottom on new output.
- **Software keyboard**: `A` opens the 3DS `swkbd` applet to compose a message;
  the draft is preserved if a request is still in flight (`Y` sends a stored
  draft).
- **HTTPS networking**: BSD sockets (SOC service) + libcurl + mbedTLS via
  devkitPro portlibs. Certificates are verified against a Mozilla CA bundle
  baked into the RomFS (`romfs/cacert.pem`) — no verification bypass.
- **Anthropic API integration**: `POST {base_url}/v1/messages` with
  `x-api-key` + `anthropic-version: 2023-06-01`, JSON built/parsed with
  cJSON. Multi-turn context: the whole conversation is resent each request
  (as the API requires).
- **Non-blocking requests**: the curl call runs on a worker thread; the UI
  stays responsive with a spinner, and `B` cancels via the curl progress
  callback.
- **Settings screen** (`SELECT`): API key (masked display), model name, and
  base URL, persisted as JSON to `sdmc:/config/claude-code-3ds/settings.json`.
- **Builds**: standard devkitPro Makefile → `claude-code-3ds.3dsx`, plus an
  optional `make cia` target using [makerom](https://github.com/3DSGuy/Project_CTR)
  (unencrypted dev CIA, installable via FBI).
- **CI**: GitHub Actions builds `.3dsx` + `.cia` in the `devkitpro/devkitarm`
  container on every push, uploads artifacts, and publishes a GitHub Release
  on `v*` tags.

### Stubbed / not implemented

- **Claude Code agent loop / tool use**: the real Claude Code runs a
  read-eval tool-use loop (file ops, shell, etc.). This client exposes **no
  tools** — it sends a plain conversation, so the model can only answer with
  text. Tool calling is the obvious next milestone.
- **Streaming (SSE)**: requests use `"stream": false`; the reply appears only
  when complete. SSE parsing via the curl write callback is straightforward
  to add.
- **Hardware keyboards**: the 3DS has no USB/BT keyboard path for homebrew —
  input is swkbd only (per design).
- **Conversation persistence**: chat history lives in RAM only; clearing (X)
  or quitting loses it. The settings file is the only persisted state.
- **Proxy/custom auth**: `base_url` is configurable, which covers simple
  API-compatible gateways, but there is no OAuth, no custom-header support,
  no `anthropic-beta` flags.

## TLS on the 3DS — read this

The 3DS *system* TLS stack (the `ssl` sysmodule used by `httpc`) is old and
weak: on older firmware it predates TLS 1.2, and even on current firmware it
offers a limited cipher list, no SNI on old versions, and a stale root store.
**This client does not use it.** TLS is done entirely in userspace by mbedTLS
over raw `soc:U` BSD sockets, so the console's TLS limitations do not apply:

- **TLS 1.2** is negotiated by mbedTLS regardless of firmware version.
  `api.anthropic.com` requires TLS 1.2+ — this is handled.
- **SNI** is sent, and the certificate chain is verified against a current
  Mozilla CA bundle baked into the RomFS (not the console's root store).
- **Cost**: the handshake runs on a 268 MHz ARM11 (Old3DS). Expect
  connect + handshake to take a few seconds on Old3DS; New3DS builds get the
  804 MHz clock when run as CIA (see `resources/template.rsf`).
- **Entropy**: TLS needs an RNG; the devkitPro mbedTLS port provides entropy
  on 3DS. Quality on hardware is a **known-risk area** — see verification
  status below.

## API key security — read before use

- The key is stored **in plaintext** at
  `sdmc:/config/claude-code-3ds/settings.json`. Anything that can read the SD
  card (any homebrew, a PC, another console) can read the key.
- The key travels to `base_url` **only** — it is never sent anywhere else,
  and the code logs nothing. TLS is verified, so it is not sniffable on the
  wire to api.anthropic.com.
- **Recommendation**: create a dedicated Anthropic API key for this app,
  set a hard spend limit on it, and revoke it when done. Do **not** point
  this at untrusted `base_url`s — that would hand your key to a third party.
- If the SD card is shared/lost, rotate the key.

## Controls

| Input | Action |
|---|---|
| `A` | Open software keyboard, compose + send message |
| `Y` | Send the stored draft (if one exists) |
| D-pad / circle pad | Scroll transcript one line |
| `L` / `R` | Page up / page down |
| `B` | Cancel in-flight request |
| `X` | Clear conversation |
| `SELECT` | Settings screen |
| `START` | Quit |

## Installing / running

Requires a 3DS with CFW (e.g. Luma3DS) — Old or New 3DS/2DS all work in
principle.

### .3dsx (Homebrew Launcher)

1. Copy `claude-code-3ds.3dsx` to `sdmc:/3ds/claude-code-3ds/`.
2. Launch via the Homebrew Launcher.

### .cia (Home Menu install)

1. Copy `claude-code-3ds.cia` to the SD card.
2. Install with FBI, then launch from the Home Menu.

### First run

Open Settings (`SELECT`), enter your Anthropic API key, save. Back on the
chat screen press `A`, type a message, send. The console must be connected
to Wi-Fi; the app performs real DNS + TLS to `api.anthropic.com` (or your
configured `base_url`).

## Building

Toolchain: **devkitARM + libctru** (devkitPro). The only non-libctru libs
used are devkitPro portlibs already present in the `devkitpro/devkitarm`
image: `curl`, `mbedtls`, `zlib`.

### Docker (recommended, matches CI)

```sh
docker run --rm -v "$PWD:/work" -w /work devkitpro/devkitarm make -j"$(nproc)"
```

### Native devkitPro install

```sh
# https://devkitpro.org/wiki/Getting_Started — install devkitARM + 3ds-dev
sudo dkp-pacman -S 3ds-dev 3ds-curl
export DEVKITPRO=/opt/devkitpro
make -j"$(nproc)"
```

Output: `claude-code-3ds.3dsx` in the repo root. `make clean` resets.

### CIA

```sh
# fetch makerom (3DSGuy/Project_CTR), then:
make cia MAKEROM=/path/to/makerom
```

Produces `claude-code-3ds.cia` (unencrypted, dev-signed — fine for CFW
installs via FBI). No banner `.bnr` is embedded, so the Home Menu top-screen
banner is blank; only the icon is set.

## Hardware verification status

**Not verified on real hardware or an emulator.** The `.3dsx` and `.cia`
compile, link, and package cleanly, but neither has been launched. Known-risk
areas to check on first hardware run:

- `swkbd` applet launch from a console-framebuffer homebrew under HBL vs CIA.
- mbedTLS handshake latency + entropy on Old3DS (268 MHz), and whether
  `api.anthropic.com`'s cert chain negotiates cleanly with the bundled CA set.
- `soc:U` socket service behavior when launched under different entrypoints
  (.3dsx via *hax payloads vs installed .cia exheader perms in
  `resources/template.rsf`).
- RomFS mount (`romfs:/cacert.pem`) under CIA — the RSF declares
  `RomFs: RootPath: romfs`, but on-console mount behavior is untested.
- Heap pressure from multi-MB API responses (capped at 4 MB) in the 64 MB
  application region.
- CIA install + boot via FBI (title ID `0x0C3D5` — change if it collides with
  another title on your console).

Emulator status: Citra forks (e.g. Azahar/PabloMK7) emulate `swkbd`, `soc:U`
sockets, and RomFS partially — enough for a smoke test, but they were **not
used** here; real TLS to the internet from an emulated 3DS is unverified.

## Layout

```
source/main.c      app loop, dual-screen UI, chat + settings screens
source/term.c      line-buffered transcript + top-screen renderer
source/net.c       curl worker thread, request/response, conversation store
source/kbd.c       swkbd wrapper
source/settings.c  SD-card settings JSON
source/cJSON.c     vendored cJSON v1.7.18 (MIT)
romfs/cacert.pem   Mozilla CA bundle (TLS verification)
resources/template.rsf  makerom descriptor for the .cia
icon.png           48x48 Home Menu icon
Makefile           devkitPro/libctru .3dsx (+ .cia) build
```

## License / attribution

- cJSON © Dave Gamble, MIT (vendored in `source/cJSON.c`, `include/cJSON.h`).
- Mozilla CA bundle © Mozilla, MPL-2.0 (`romfs/cacert.pem`).
- makerom © jakcron/3DSGuy (Project_CTR) — used only in CI packaging.
- Everything else: MIT. Homebrew for interoperability; not affiliated with
  Anthropic or Nintendo.
