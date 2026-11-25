# 02 — Root access

How root was gained on the 58A51HUV, what that left on the TV, how to
connect, how to update it and how to take it out. Everything was done on
2026-11-25 with the owner at the TV for each step that changed what it runs.

The full measurement log is the alvidaa model guide,
`docs/models/58A51HUV/V0000.06.30N.N0907/README.md` in that repo (commits
`5cab4e7` and `5c6082d` on branch `58a51huv-u6-launcher-lane`).

## The chain

Four facts, each measured:

1. **The browser's privileged functions are gated on origin.** A page on
   `https://<anything>.vidaahub.com` sees 102 `Hisense_*` functions, among
   them `Hisense_FileRead` and `Hisense_FileWrite`. Any plain-HTTP origin
   sees 74 and none of those. A self-signed certificate passes once the
   owner clicks through the browser warning. So: pin one `vidaahub.com` name
   to a PC with a local DNS responder, serve the page over HTTPS from there.
2. **`../` escapes the file bridge.** `Hisense_FileRead("../etc/rc.d/rc.local", 6)`
   returns the script. Mode 6's base is one level below `/`. The bridge runs
   as `UhisWebsdk` (uid 314), so it reads and writes what that user can.
3. **`rc.local` runs `/var/local/otaTest.sh` as root at every boot**, in the
   background, if the file exists. The vendor's own comment calls it an OTA
   auto-test interface.
4. **`/var/local` is flash, writable by the bridge, and not `noexec`**
   (`mmcblk0p39`, ext4, world-writable directory).

Put together: a page writes a script to `/var/local/otaTest.sh`; the next
boot runs it as uid 0 with full capabilities. Verified boot is not involved,
because nothing on a verified partition changes.

## What the hook does now (v3)

`/var/local/otaTest.sh`, 5,186 bytes, at every boot:

1. Exits at once if `/var/local/hook.off` exists.
2. Re-executes itself under `busybox setsid`, so it no longer belongs to
   `rc.local`'s session.
3. Checks the md5 of `/var/local/tvtools/dropbearmulti`. If it is wrong,
   rebuilds the binary from 38 base64 chunk files (`/var/local/alvidaa-db.0`
   to `.37`: decode, gunzip, md5 again) and starts nothing if that fails.
4. Writes the owner's public key to `/var/local/roothome/authorized_keys`,
   makes an ed25519 host key on first run.
5. Starts `dropbear -F -E -p 2222 -r <hostkey> -D /var/local/roothome -s`.
6. A watchdog, every 15 s for three minutes and every 30 s after: keeps one
   `iptables` ACCEPT for tcp/2222 as rule 1 of `INPUT` (at most 40
   re-inserts a boot), restarts the listener if no `dropbear` process is
   left (at most 20 times), and on `hook.off` stops the listener and exits.
7. Writes a read-only inventory of the boot to
   `/var/local/alvidaa-hook-ls.log`.

Timeline of the one boot observed: hook at uptime 5.3 s, binary check and
`dropbear -V` at 6 s, listener up at 8 s, firewall rule in at 24 s, Wi-Fi up
some time before 96 s.

## The daemon

Dropbear 2026.94, static armv7 (musl, static-pie), one multi-call binary of
631,604 bytes. Four source patches, each for a property of this firmware:

| Patch | Why |
| --- | --- |
| skip the `/etc/shells` check | root's shell is a gate script and there is no `/etc/shells` |
| `get_user_shell()` returns `/bin/sh` | or every login runs the USB-key gate and is closed |
| skip the key-path permission walk | `/var/local` is world-writable by vendor design |
| forget a failed PTY request | no PTYs here; stock Dropbear refuses the shell after a refused PTY |

Password auth is compiled out. The root `PATH` is baked in:
`/usr/local/bin:/usr/local/scripts:/usr/sbin:/usr/bin:/sbin:/bin:/opt/bin:/var/local/tvtools`.

## Connect

```sh
TV_HOST=192.168.2.152 ./server/tv 'busybox id'
TV_HOST=192.168.2.152 TV_NOPTY=1 ./server/tv
ssh -p 2222 -i server/keys/tv_root_ed25519 -o UserKnownHostsFile=server/keys/tv_known_hosts root@192.168.2.152
scp -O -P 2222 -i server/keys/tv_root_ed25519 file root@192.168.2.152:/var/local/
ssh -p 2222 -i server/keys/tv_root_ed25519 root@192.168.2.152 'sh -s' < script.sh
```

- TV `192.168.2.152`, PC `192.168.2.154` when this was written. Both DHCP.
- No `-t`. OpenSSH exits 255 when an explicit PTY request is refused.
- A plain interactive `ssh` prints `PTY allocation request failed on channel 0`
  and then gives `sh-3.2#`. Echo and line editing are the client's.
- `scp` needs `-O` (no SFTP server on the TV).
- PuTTY: untested since the PTY patch; expect to need Terminal → "Implicit
  CR in every LF".
- Windows `ssh.exe` refuses a key other accounts can read. The repo folders
  on this PC grant `Everyone` full access, so the key file in each checkout
  was restricted to the owner's account by hand. A fresh copy needs that
  again: `icacls server\keys\tv_root_ed25519 /inheritance:r /grant:r %USERNAME%:(F)`.

## What is on the TV

All under `/var/local`:

| Path | What | Owner |
| --- | --- | --- |
| `otaTest.sh` | the hook | `UhisWebsdk` |
| `otaTest.prev.bak` | the hook before the last change | `UhisWebsdk` |
| `alvidaa-db.0` … `.37` | the binary as base64 chunks | `UhisWebsdk` |
| `tvtools/` | `dropbearmulti`, symlinks `dropbear` `dropbearkey` `scp`, the host key | root |
| `roothome/authorized_keys` | the owner's public key | root |
| `alvidaa-hook.log`, `alvidaa-hook-ls.log`, `alvidaa-dropbear.log` | logs, rewritten each boot | root |
| `alvidaa-marker.txt` | one line from the first write test | `UhisWebsdk` |

Runtime only: the `iptables` rule and `/var/run/alvidaa-dropbear.pid`.

Outside `/var/local`: one entry (`vidaa-home`) in `/APPS/websdk/Appinfo.json`,
the launcher tile registered through the vendor's own `Hisense_installApp`.

## Stop it, remove it

| Want | Do | Tested |
| --- | --- | --- |
| stop SSH now, keep it off | `touch /var/local/hook.off` from the shell: listener stops within 30 s, nothing starts at boot | on a PC with a fake binary only |
| turn it back on | `rm /var/local/hook.off`, reboot | no |
| remove everything | `rm -rf /var/local/otaTest.sh /var/local/otaTest.prev.bak /var/local/tvtools /var/local/roothome /var/local/alvidaa-* /var/local/hook.off`, reboot | no |
| disarm without a shell | the private pages' REPLACE WITH NO-OP button overwrites the hook with `exit 0` | page logic tested against a fake bridge only |

## Updating the daemon

Done once (the PTY patch), without a reboot and without the browser:

1. `scp` the new binary beside the old one.
2. Start it on a spare port behind a temporary ACCEPT rule placed as rule 2,
   and exercise it there. This is also the way back in if step 4 goes wrong.
3. Write the new chunks and the new hook **in place** (`cat new > file`), so
   the files the bridge created stay owned by `UhisWebsdk`. Check each md5.
4. `mv` the binary into place, kill the old listener, start the new one with
   the hook's own command line.
5. Verify on 2222, then stop the spare listener and delete its rule.

The hook that is on the TV now differs from the one that booted only in its
md5 and install stamp. It has not been through a boot.

## Getting back in from nothing

If SSH and the hook are both gone, the chain above has to be run again from
the browser. (Whether a factory reset clears `/var/local` is not known.) The
tooling is not in this repo:

| Piece | Where (alvidaa checkout) |
| --- | --- |
| DNS pin, HTTPS probe server | `tools/dns-pin.py`, `tools/fingerprint-serve.py --tls --dir=…` |
| hook pages (write probe, proof, inventory, SSH) | `research/58a51huv/www/` (gitignored) |
| payload packer | `research/58a51huv/pack-ssh.py` (gitignored) |
| built binary | `research/58a51huv/tvtools/dropbearmulti` (gitignored) |
| build recipe and patches | `tools/58a51huv/dropbear-build.sh`, `tools/58a51huv/dropbear-nopty.patch`, `tools/a6q/dropbear-aarch64/dropbear-tv.patch` |

Sequence: start the two servers on the PC, set the TV's DNS to the PC, open
`https://alvidaa.vidaahub.com:8444/u6-root-ssh.html`, accept the certificate,
press DELIVER then INSTALL, power-cycle, set the DNS back.

## Not known

- Whether an OTA update removes `rc.local`'s `otaTest.sh` stanza or the
  origin gate. No OTA has run since the hook went in, and nothing here
  blocks one yet.
- Whether the vendor's own `deploy` step, which runs at every boot, ever
  touches `/var/local/otaTest.sh`. It did not across three boots.
