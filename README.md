# vidaa-vibe

Replace the stock VIDAA user interface on one Hisense TV, our own, with our
own code. VIDAA OS is buggy and tedious day to day; the aim is a UI we
control end to end.

This grew out of work in the [alvidaa](https://github.com/carteakey/alvidaa)
launcher project, which puts a home screen on top of VIDAA. This repo goes
further: the stock UI is the thing being replaced.

## Status (2026-11-25)

| Step | State |
| --- | --- |
| Root on the TV | done: key-only root SSH on tcp/2222, started by a boot hook |
| Map of the system | done: partitions, boot chain, processes, firewall |
| Map of the UI stack | done for the outside: engine, bundle, A/B slots, input, display |
| The UI's own native API | not mapped |
| Native route (own DirectFB client) | works: `native/vibe` joins the UI's DirectFB world, shows a window, gets focus and remote keys |
| TV functions from native code | inputs, volume, mute through the vendor's model facade; POWER (standby and wake) handled by the system once our UI reports itself loaded |
| Our UI at boot | `vibe-ui` starts from the boot hook about 14 s into boot; the TV restores the last input by itself |
| Anything replaced | the stock UI boots from slot B as an empty page (no Hisense UI on screen); slot A untouched; way back in [06](docs/findings/06-native-route.md) |

## The unit

Hisense 58A51HUV, firmware `V0000.06.30N.N0907`, VIDAA U6 (`U06.30`),
MediaTek MT9602, armv7l, Linux 4.19.79. One unit, ours.

## Findings

Everything here was measured on that unit unless a line says otherwise.

| File | What |
| --- | --- |
| [01-unit.md](docs/findings/01-unit.md) | hardware, firmware, runtimes, what tools exist on the TV |
| [02-root-access.md](docs/findings/02-root-access.md) | how root was gained, how to connect, what is installed, how to remove it |
| [03-system-map.md](docs/findings/03-system-map.md) | partitions, boot chain, processes, listeners, firewall |
| [04-ui-stack.md](docs/findings/04-ui-stack.md) | what draws the UI, where its code lives, how it could be replaced |
| [05-traps.md](docs/findings/05-traps.md) | every trap hit or read about, one line each |
| [06-native-route.md](docs/findings/06-native-route.md) | DirectFB world, the vendor ABI, our first native window |

Raw command output behind these files is in `research/58a51huv/recon/`
(gitignored: it quotes vendor scripts).

## Connect

```sh
TV_HOST=192.168.2.152 ./server/tv 'busybox id'      # one command
TV_HOST=192.168.2.152 TV_NOPTY=1 ./server/tv        # interactive, no terminal
ssh -p 2222 -i server/keys/tv_root_ed25519 root@192.168.2.152
```

The address is a DHCP lease; re-check it. There is no PTY on this TV: see
[05-traps.md](docs/findings/05-traps.md).

## What is not in this repo

- Keys, tokens and reports that carry unit identifiers: `server/keys/` and
  `server/recon-report*.json` are gitignored.
- The pages and scripts that install the boot hook, and the built SSH
  binary. They live in the alvidaa checkout under its gitignored
  `research/58a51huv/`; [02-root-access.md](docs/findings/02-root-access.md)
  says where.

## Ground rules

[AGENTS.md](AGENTS.md). Short version: our TV only, every change has a way
back, and verified boot stays green.
