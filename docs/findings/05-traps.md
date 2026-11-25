# 05 — Traps

One entry per trap. "Hit" means it cost time on this TV on 2026-11-25.
"Read" means it is recorded for a sibling model in the alvidaa repo and was
not reproduced here. "Script" means it follows from a vendor script's text
and was not observed.

## Could lock us out or wipe work

| Trap | Source | What to do |
| --- | --- | --- |
| The SSH daemon is the only recovery path that needs no one at the TV | — | Change it only with a second listener already up on a spare port |
| Files the browser bridge created are owned by `UhisWebsdk`. Replace one (`mv`, `sed -i`, `cp` over a new inode) and it becomes root's; the bridge can then no longer overwrite the hook, which is the disarm of last resort | by design, avoided | Rewrite in place: `cat new > file`. Check with `ls -la` |
| The vendor's OTA routine deletes the **active** UI slot and re-extracts the factory bundle when the slot's `versionInfo.json` is missing or not newer than the factory one | Script | Give our bundle a higher `version`. Decide what to do about OTA before relying on a slot |
| Boot resets owner and mode of `/APPS/channelA` and `/APPS/channelB` (`chown -R UhisApp:UGhisApp`, `chmod 750`) | Script | Do not depend on other ownership inside a slot |
| `/var/tvservice/reset_working` starts a factory reset at boot; `/OAD` is the OTA lane; `/var/local/oem` is copied into system partitions by a root script; `/APPS/pkgs/NewUpdatedIPK.txt` runs scripts as root | Read | Never create or write these |
| A root-owned `.starboard.*.storage` in the engine's home breaks the UI, and recreating it loses the Home layout | Read | Never run the engine, or touch its state, as root |
| An unanswered PIN dialog turns every MQTT `gettvstate` into a new PIN dialog | Hit | No `sendkey`, no polling watcher, no `authenticationcodeclose` on this TV. Recovery: stop all MQTT traffic, let the dialog time out, re-pair |
| `authenticationcodeclose` leaves a modal input grab in the UI; a malformed `/file/*` request wedges UI input | Read (65U8HAU, U6) | Arguments to the file bridge are JSON strings, always |
| `/hiSecurity/*` POSTs killed the file handler on another U6 unit | Read (65U7H) | Never call them |
| `Hisense_ResetDevice`, `Hisense_SetDNS`, `Hisense_setDebugPort` sit next to the file functions on the HTTPS origin | Hit (seen, not called) | Call bridge functions by name, one at a time; never enumerate-and-call |

## Cost a boot cycle or an hour

| Trap | Source | What to do |
| --- | --- | --- |
| `Hisense_FileRead` stops at the first NUL byte. `/proc/self/attr/current` ends in one, and a log that contained it read back as 141 characters | Hit | Nothing that can emit a NUL goes into a file the bridge has to read |
| `tr` does not exist. A hook that piped listings through it logged nothing | Hit | Check a tool exists before a boot depends on it: [01-unit.md](01-unit.md) |
| A hook's later output is not there yet if the log is read too soon; a 90 s pass looked like a dead hook | Hit | Put an end marker in every log and check for it |
| No PTYs: no UNIX98 support in the kernel, and legacy `ptyp*` pairs made with `mknod` open, accept writes and deliver nothing | Hit | No terminal programs. Do not spend time on PTYs again |
| Stock Dropbear refuses the shell after a refused PTY request (PuTTY: "Server refused to allocate pty", then "Server refused to start a shell/command") | Hit | Our build carries a patch. Any rebuild must keep it |
| OpenSSH exits 255 when a PTY asked for by flag (`-t`) is refused, whatever the server does next | Hit | Never pass `-t`. `TV_NOPTY=1 ./server/tv` |
| Reading a device or fifo can block for good: `read -t 3` did not time out on a pty master and the session hung | Hit | Give such commands a background watchdog that kills the shell |
| The clock reads 1999-12-31 when the hook starts, and the TV ran about 18 minutes behind the PC later | Hit | Never compare TV timestamps with PC time; `tar` will warn about files from the future |
| A file root creates under the boot umask (`0026`) is `0640`: the bridge cannot read it | Hit | `umask 022` at the top of any hook |
| The privileged browser functions exist only on `https://*.vidaahub.com`; every plain-HTTP origin gets none, with no error | Hit | DNS pin plus self-signed HTTPS. Port and hostname within the domain do not matter |
| `launchapp` over MQTT ignores URLs on this TV; it only starts registered apps | Hit | Register a tile with `Hisense_installApp`, restart the TV, launch it by id |
| A registered tile does not show until the TV restarts | Hit | Restart after changing `Appinfo.json` |
| ICMP is accepted by the firewall chain here, but sibling firmware does not answer ping | Read | Probe TCP 36669 or 2222, not ping |

## Native code (DirectFB)

| Trap | Source | What to do |
| --- | --- | --- |
| The vendor libdirectfb has extra slots in the middle of its interface tables; upstream 1.4.2 headers call the wrong functions (`EnumScreens` ran `CreatePalette` and corrupted the interface) | Hit | Build only with `native/`'s patched headers; check new methods with `native/tools/check_abi.py` |
| An enumeration callback's `callbackdata` arrives as garbage: the description passed by value is larger than upstream's | Hit | Use a global, never `ctx`; give library-filled structs slack |
| `GetWindow(id)` answers OK with the same data for any id | Hit | Do not use it to list windows |
| The library is Thumb-2; `llvm-objdump` in ARM mode prints plausible garbage | Hit | `--triple=thumbv7` |
| Zig's default loader for `arm-linux-gnueabihf` is `/lib/ld-linux-armhf.so.3`, absent here | Hit | Link with `-Wl,--dynamic-linker=/lib/ld-linux.so.3` |
| A DirectFB client started without `session=0` may create a new world and become master, reinitialising the display hardware | Script (upstream DirectFB behaviour), avoided | Always `session=0` and `force-slave` |
| `busybox tee` is not an applet here | Hit | Redirect to a file, then `cat` it |
| BACK and some unnamed keys arrive twice: the second copy comes from the IR device (15) with `key_code` −1 | Hit | Drop a repeat with code −1 |
| Volume and mute keys act on the TV even while our window has focus (and receives them too), **but only because the stock UI does it**. With the stock UI blanked (slot B), nothing changes the volume | Hit | Our app sets `kilby.sl2.i32.sound.main.volume` / `.mute` itself |
| With a soundbar on ARC, writing `sound.main.volume` does nothing to the soundbar but changes the reading (echo), which then jumps to the soundbar's real level | Hit | In ARC mode write nothing; the system forwards the keys over CEC. Only display |
| `Lock(DSLF_READ)` on the window surface returns zeros | Hit | Read back with `DSLF_READ | DSLF_WRITE` |
| Rewriting a running shell script in place (`cat > boot.sh`) can make bash execute garbage, because it reads the file as it runs | Avoided | `native/deploy.sh` writes `X.new` and renames it over `X` |
| A shared log file for every tool (`last.log`) let a tool run truncate the running UI's log (NUL bytes in it afterwards) | Hit | One log per binary: `last-<binary>.log` |
| `tail`, `tr`, `uniq` are missing too; piping a script into a missing command killed the script with SIGPIPE before it did anything | Hit | Use `$BB tail`; avoid pipes into unverified commands |
| `cut` is not on the TV's PATH (busybox only); a wait-for-SSH loop that ran `cut` remotely read every answer as "down" | Hit | Use `$BB cut` in remote commands, or test the port (`bash -c 'echo > /dev/tcp/HOST/2222'`) |
| Files **created** on `/var/local` shortly before a mains power cut came back **empty** (the slot B backups, 276 → 0 bytes); files rewritten in place survived. An unchecked restore then emptied `appRunTimeInfoFile.json` | Hit | `sync` after every write that matters; check a backup's md5 before restoring from it |
| `kilby.action.system.switch.off.tv` puts the TV into a fast standby that, with the stock page blanked, POWER does not wake | Hit | Only with `kilby.sl2.i32.system.gui.loop.finished` = 1 (vibe-ui sets it at start): then POWER wakes it (tested). POWER itself is left to the system; vibe-ui sends the action only at sleep-timer end |
| A replacement UI that never reports itself loaded (`gui.loop.finished` stays 0) gets no working POWER: the system ignores the key | Hit | Set it to 1 at start, as the stock SDK's `uiLoadFinish(1)` does |
| Model action names sit next to destructive ones (`kilby.sl2.action.system.factory.reset`, `...clear.user.data`), and some keys drop the `sl2` part (`kilby.action.system.switch.off.tv`) | Read | Never guess an action name; take it from the engine's code |
| `libbizAgent.so`'s string getters `strcpy` with no length | Read in its code, avoided | Large, zeroed buffers (`vibe-model` uses 8 KiB per string) |
| `libbizAgent.so` is ARM code while `libdirectfb` is Thumb-2 | Hit | Check both modes when disassembling vendor libraries |
| `grep -o` with wide `.{0,N}` context on the bundle's one-line minified JS runs for minutes on the TV's CPU (load ~47) and timed out | Hit | Copy files to the PC and search there |

## On the PC

| Trap | Source | What to do |
| --- | --- | --- |
| The repo folders grant `Everyone` full access, so Windows `ssh.exe` rejects the private key ("UNPROTECTED PRIVATE KEY FILE") | Hit | `icacls server\keys\tv_root_ed25519 /inheritance:r /grant:r %USERNAME%:(F)` in each checkout |
| `core.autocrlf=true` checks `.sh` and `.patch` files out with CRLF; `patch` rejects them and WSL bash will not run them | Hit | Strip `\r` before use; keep scripts that run on the TV LF-only and ASCII |
| A script sent to the TV with CRLF or non-ASCII bytes fails in ways that look like logic errors | avoided | Pipe scripts as files (`ssh … 'sh -s' < file`), not as quoted one-liners |
| `wsl.exe -- cmd` expands `$vars` in the default shell first; and WSL here cannot reach the TV's LAN address at all | Hit | `wsl.exe -e bash -c '…'`; talk to the TV from Windows, build in WSL |
| `python3` on this PC is the Store stub | Hit | `python`, with `PYTHONUTF8=1` |
| `pkill -f <pattern>` inside `sh -c '…<pattern>…'` kills its own shell | Hit, twice | Do not put the pattern in the command line that runs `pkill` |
| Quoted heredocs passed through some tools lose a level of backslashes | Hit | Write the script to a file first |

## UI engine, from the U9 work

All "Read"; see [04-ui-stack.md](04-ui-stack.md) for the list (no canvas or
iframe, stale compiled-script cache, CORS-preflight deadlock, scripts not
retried before the network is up, gray screen from a wholesale `index.html`
replacement).
