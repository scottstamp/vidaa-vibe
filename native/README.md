# native/ — our own DirectFB client

A native UI for the 58A51HUV, drawn with the TV's own DirectFB, no web
engine. Background and measurements:
[docs/findings/06-native-route.md](../docs/findings/06-native-route.md).

## What is here

| Path | What |
| --- | --- |
| `src/vibe.c` | the client: `probe` (read-only listing) and `hello` (one window, key log) |
| `src/vibe-model.c` | the TV's model service through `libbizAgent.so`: get keys, list inputs, `--yes switch-input HDMIn` |
| `src/vibe-ui.c` | the app skeleton: full-screen transparent window, input panel (HOME/MENU/INPUT), input switching, volume indicator |
| `src/gfx.c` | software shapes for the UI: anti-aliased rounded rectangles, gradients, glows, drawn into the locked window surface |
| `src/gam.c` | `app_man` (GAM) client, shared by `vibe-gam` and `vibe-ui --gam` |
| `src/biz.c` | the model facade (`libbizAgent.so` via dlopen): typed getters, input list, `switch_input` |
| `src/common.c` | shared helpers (privilege drop, clock, `PADDED`) |
| `tv/vibe-run.sh` | on-TV launcher: `hisenseUI`'s library path, drops to UhisUI's identity |
| `build.sh` | build in WSL with zig, target armv7 hard-float glibc 2.21 |
| `pull-sysroot.sh` | copy the TV's three DirectFB libraries into `.deps/sysroot` (Windows side) |
| `deploy.sh` | stream `out/vibe`, `out/vibe-model` and the launcher to `/var/local/vibe/` (Windows side). Starts nothing |
| `src/vibe-gam.c` | `app_man` client (MediaTek GAM, `/basic/libgam_client.so`): process list and states, `--yes cmd am stop APP` |
| `slotb/` | the blank stock-UI bundle for slot B and `install.sh` (`SLOTB_STEP=stage|flip|unflip`); see the findings, "Slot B" |
| `tools/mstar_headers.py` | patch upstream DirectFB 1.4.2 headers to the vendor's interface layout |
| `tools/modeljs_keys.py` | map stock `modeljs` methods to the model keys they use (from `libcobalt.so`) |
| `tools/tvshots.sh`, `tools/shot2png.py` | screenshots of `vibe-ui` through its dev hook, as PNG |
| `tools/vtables.py`, `tools/check_abi.py` | recover the vendor layout from the library and check a header against it |

`.deps/` (fetched headers, vendor libraries) and `out/` are gitignored.

## Build and run

```sh
TV_HOST=192.168.2.152 native/pull-sysroot.sh     # once, Git Bash
wsl.exe -e bash native/build.sh                   # WSL cannot reach the TV
TV_HOST=192.168.2.152 native/deploy.sh            # Git Bash
ssh … root@TV '/var/local/vibe/vibe-run.sh probe'
ssh … root@TV '/var/local/vibe/vibe-run.sh --timeout=20 hello'   # visible: ask first
ssh … root@TV 'VIBE_BIN=vibe-model /var/local/vibe/vibe-run.sh inputs'
ssh … root@TV 'VIBE_BIN=vibe-model /var/local/vibe/vibe-run.sh --yes switch-input HDMI2'  # visible
ssh … root@TV 'VIBE_BIN=vibe-ui /var/local/vibe/vibe-run.sh --timeout=120'                   # visible: the app
```

`build.sh` downloads DirectFB-1.4.2 from the buildroot mirror once,
checks its sha256 and uses only its public headers.

## Rules for code in here

- **Never call through upstream DirectFB headers.** Build only with the
  patched headers `build.sh` makes. A new interface method must be checked
  against the library first: run `tools/vtables.py` and `tools/check_abi.py`
  (see the findings file for the commands' inputs).
- Do not call `_mstar_*` slots.
- Give every struct the library writes into slack (`PADDED`), and never use
  an enumeration callback's `callbackdata`: by-value descriptions move it.
- Join world 0 as a slave only (`session=0`, `force-slave`). Run as UhisUI's
  identity, never as root.
- Anything visible on the TV needs the owner's go-ahead (AGENTS.md).

## Development hook

With the root-owned flag `/var/local/vibe/dev` present when `vibe-ui`
starts, it reads `/tmp/vibe-cmd` every 0.3 s: `key 0xf005` acts as that remote
key (HOME here), `shot` writes the next frame (our layer only, ARGB) to
`/tmp/vibe-shot.raw`. `tools/tvshots.sh` drives this and converts the frames
to PNG under `research/shots/`. Remove the flag to turn the hook off.
