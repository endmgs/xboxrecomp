# Xbox Live (Insignia) with a recompiled game

[Insignia](https://insignia.live/) is a closed-source revival of the original
Xbox Live, which Microsoft shut down in 2010. This guide is for players and
for toolkit developers. The technical record is `docs/technical/xbox-live.md`.

## 1. Summary and status

A recompiled game contains its own Xbox Live client (XOnline, lifted with
the game). The runtime gives it what a console would: a network card with a
cable, a router, the console's identity, and the accounts on its disk. With
these, Rainbow Six 3 on macOS sends a Kerberos sign-in request and Insignia
answers with a ticket: it accepts the console's identity, registration and
machine account. **The game does not yet process that answer, so nobody
can sign in yet.** That is the open problem.

| Piece | State |
| --- | --- |
| Network card on Windows and macOS/POSIX | done |
| Router in the runtime (`syslink = nat`) | done; `tests/net_nat` |
| Console identity from the player's backup | done; `tests/console_id` |
| EEPROM block reads answer the real EEPROM | done |
| Accounts copied onto the disk image | done |
| Xbox Live block in hard-drive copies undone | Rainbow Six 3 only (`scripts/live_unpatch.py`) |
| DNS lookup, Kerberos request, Insignia's reply | works |
| Game processes the reply | **open** |
| Ticket for the service, security gateway, matchmaking | not reached |

## 2. For players

### What you need

- **Your own original Xbox, softmodded** (a save-game exploit that runs
  homebrew such as the EvoX dashboard), to back up its identity.
- **An Insignia subscription code** from insignia.live, and an email address.
- **Your own game files**, built with this toolkit.
- **FTP** to copy files off the console (EvoX, UnleashX and XBMC serve it).

Terms used below:

- **EEPROM**: a 256-byte chip holding the console's serial number, Ethernet
  address and **online key** (the secret Xbox Live knows the console by).
- **HDD key**: a per-console key that signs data on the hard disk. Insignia
  refuses a console whose HDD key is all zeros.
- **Machine account / user account**: the console's own account
  (`SN.<serial>`) and your gamertag's. Both live on the hard disk.

### Step 1: register and sign up, on the real console

Do this once, on the console itself.

1. If your modded BIOS blocks Xbox Live, run Insignia's **xblunblock** first.
   It patches memory only, so it is gone at power-off.
2. Run the **Insignia Setup Assistant** to register the console.
3. In the dashboard's Xbox Live section, create the account with your
   subscription code and the same email.

### Step 2: back up the console

1. Run `E:\backups\eeprom\evoxdash.xbe` and choose **Backup eeprom** in
   its menu. It writes `eeprom.bin`, `hddinfo.txt` and `disk.bin` (the
   drive's first 512 KB, which holds the accounts).
2. Back up **after** Step 1. Rename any older EvoX backup folder first, or
   EvoX may not write a new one.
3. Copy the files off over FTP.

### Step 3: put the files in `xboxbackup/`

Create a folder `xboxbackup/` beside the game folder:

```text
Rainbow-six-three-recomp/
  game/            your game files
  xboxbackup/      your console's identity (never commit or share)
    eeprom.bin
    hddinfo.txt
    disk.bin
```

| File | Needed | What it gives |
| --- | --- | --- |
| `eeprom.bin` | yes | serial, Ethernet address, online key (checksums checked) |
| `hddinfo.txt` | for the HDD key | its `XboxHDKey` line, the drive's serial and model |
| `disk.bin` | for accounts | the machine and user accounts |
| `bios.bin` | optional | not read today; for a future HDD-key tool |

Make it private (`chmod 700 xboxbackup`); the game project's `.gitignore`
already excludes it. Alternatives: `console_dir = <folder>`
(`RECOMP_CONSOLE_DIR`), or `console/` in the per-user settings folder.
`console_dir = off` disables the identity.

### Step 4: check the HDD key

The runtime takes the first HDD key it finds from: `RECOMP_XBOX_HD_KEY`,
then `hd_key = <32 hex digits>` in `keys.ini` (per-user settings folder),
then the `XboxHDKey` line of `hddinfo.txt`, then `hd_key.txt`.

**EvoX's `hddkey.bin` is the drive's lock password, not the HDD key.** To
cross-check, Ryzee119's xbox_eeprom_online (runs entirely in your browser)
shows the key from `eeprom.bin`. Paste keys nowhere else.

### Step 5: run with the router on

Set `syslink = nat` in the title's settings file (macOS:
`~/Library/Application Support/xboxrecomp/titles/<title id>.conf`), or
`RECOMP_SYSLINK=nat`. The DNS servers default to Insignia's (46.101.64.175)
then 1.1.1.1; `net_dns` changes them. Then use the game's Xbox Live menu.

### What the log lines mean

| Line | Meaning |
| --- | --- |
| `[XBOX] console identity from <dir> (beside the game folder): eeprom.bin, Ethernet address XX:XX:XX:..., HDD key from hddinfo.txt` | identity loaded; only the maker prefix is printed |
| `[XBOX] Xbox Live accounts copied from disk.bin onto the disk (sectors 8-15): 1 machine account, 1 user account` | accounts imported (first run only) |
| `[NET] internet through this PC: the console is 10.0.2.15 behind the router at 10.0.2.2, DNS 46.101.64.175 then 1.1.1.1` | the game took its address |
| `[NET] DNS: AS.XBOXLIVE.COM, asked of 46.101.64.175` | sign-in has started |
| `[NET] UDP from the console's port 3074 leaves this PC from port <n> (its own was taken)` | something else on this PC holds 3074 |

### Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| "Cannot connect to Xbox Live" at once, no `[NET] DNS` lines | a hard-drive copy whose `default.xbe` has the Live block | lift a copy with it undone (Rainbow Six 3: `scripts/recompile.sh` does it); keep `default.bak` |
| `accounts: none in .../disk.bin` | backup made before sign-up | sign up, rename the old EvoX backup folder, back up again |
| Gamertag missing in game | same | same, then `console_accounts = replace` once |
| `fails its checksums` | not an EEPROM dump, or damaged | back up again |
| `no HDD key` or `HDD key is all zeros` | key not found, or `hddkey.bin` used | Step 4 |
| Two copies cannot see each other in System Link | both load one identity | `console_dir = off` on one |

### Playing fair

Insignia's Terms of Use forbid probing, fuzzing, overloading or unauthorised
access. Behave as an ordinary client and keep logs and captures private.

## 3. What was built

| Component | File | Switches |
| --- | --- | --- |
| Network card on POSIX: register access trapped by the fault handler (arm64 decoder), guest memory through a backdoor; with the card on, `PhyGetLinkState` reports a cable | `src/kernel/xbox_nic.c` | `syslink` (`RECOMP_SYSLINK`) |
| Router: DHCP (console 10.0.2.15, router 10.0.2.2), ARP for the router, UDP out keeping the console's port when free, IP fragments both ways, ICMP echo to the router, TCP refused with a log line | `src/kernel/xbox_net_nat.c` | `syslink = nat`, `net_dns`, `RECOMP_NET_LOG`, `RECOMP_NET_PCAP=<file>`, `RECOMP_NET_FRESH_PORTS` |
| Console identity: finds and validates the backup, resolves the HDD key, handles a symlinked game folder | `src/kernel/xbox_console_id.c` | `console_dir`, `RECOMP_XBOX_HD_KEY`, `keys.ini` |
| EEPROM block reads (`XC_MAX_ALL` 0xFFFF, `XC_MAX_OS` 0xFF, `XC_MAX_FACTORY` 0x1FF) answer the player's EEPROM with the runtime's user settings laid over it | `src/kernel/kernel_xbox.c` (`console_eeprom`) | none |
| Account import: sectors 8-15 of `disk.bin` onto `Partition0.img` in the save folder, once, when the image has no account | `xbox_console_import_accounts` | `console_accounts = import / replace / off` |
| Live-block undo for one title | `Rainbow-six-three-recomp/scripts/live_unpatch.py` | run by `scripts/recompile.sh` |

On the disk: sector 8 is an index, sector 9 the machine account (domain
`insignia.live`), the user account later (12 in the one backup seen).

**Diagnostics.**

- `RECOMP_NET_PCAP=<file>`: every frame, for Wireshark. Holds sign-in
  traffic: keep private.
- `RECOMP_XC_TRACE=1`: each kernel crypto call, EEPROM query and
  `PhyGetLinkState` (caller, lengths, results, short fingerprints; never
  data). `RECOMP_XC_TRACE_BUDGET` caps it (default 3000 lines).

No log line contains a secret.

## 4. Lessons learned

1. **Hard-drive copies block Live.** "HDD-ready" copies patch `default.xbe`
   (original kept as `default.bak`): allowed media set to any, a media-check
   `jge` to `jmp` at 0x000A05FA, and at 0x0038C6FD `test al,1; jnz` replaced
   by four `nop`s. That last one makes `XOnlineLogon` fail with 0x80151000
   (believed to be `XONLINE_E_LOGON_NO_NETWORK_CONNECTION`) without sending
   a packet. "No traffic at all" points here first.
2. **Single settings were not enough.** XNet reads the whole EEPROM in one
   block at start-up; those reads returned zeros while each single query
   answered correctly.
3. **`hddkey.bin` is not the HDD key.** It is the drive's lock password.
4. **The 500-byte HMAC checks are the cache partitions' signatures** (keyed
   by the HDD key), not the accounts'. A start-up "HDD key does not verify"
   check built on the wrong reading was removed.
5. **A backup from before sign-up has no user account**, and EvoX may keep
   the old backup unless its folder is renamed.
6. **No TCP needed.** Xbox Live tunnels TCP inside UDP 3074.

## 5. Proposed automation

Effort: S = days, M = one to two weeks, L = longer. Nothing here may ship a
player's keys or any Microsoft key; items marked **local** handle secrets and
must never send data off the PC.

### Must

| Item | What it does | Effort | Where |
| --- | --- | --- | --- |
| Live-patch detector and undoer | For every title, compare `default.xbe` with `default.bak`, or match known byte signatures per title; lift a copy with the Live block undone; report it. Replaces per-project scripts. Never edits the game's files. | M | `scripts/recompile.py` stage, signatures in `tools/xbe_parser/` |
| Backup importer (**local**) | Accept an EvoX, UnleashX or XBMC backup folder, or pull it from the console over FTP; copy only `eeprom.bin`, `hddinfo.txt` and the first 512 KB of `disk.bin`; check checksums; set private permissions. | M | `tools/console_import/`, called by the launcher |
| Doctor | One command that checks every prerequisite and prints a checklist: identity found, checksums, HDD key source, accounts present, Live patch status, `syslink = nat`, DNS reachable. Built on the runtime's own C code so it agrees with it. | S | `--live-check` flag in the runtime |
| Launcher "Xbox Live" tab | Pick the backup folder; show identity status without secrets, accounts found (counts only), HDD key source, Live patch status, network mode `nat`, DNS; a **Test connection** button that only resolves a name and checks reachability. | M | `src/launcher/launcher.c`, new tab beside SYSTEM LINK |

### Should

| Item | What it does | Effort | Where |
| --- | --- | --- | --- |
| HDD-key resolver (**local**) | Decrypt the HDD key from `eeprom.bin` with the EEPROM key read from the player's own `bios.bin`. Ships no keys. | M | `xbox_console_id.c` |
| Account refresh and memory-card import | A "refresh accounts" action (`replace` once); import accounts from a memory unit dump. | S/M | `xbox_console_id.c`, launcher |
| Local Xombie profile | A development profile pointing DNS at a local [Xombie](https://github.com/xombieonline/xombie) (AGPL) server, so sign-in can be tested without touching Insignia. | M | `net_dns` profile, `docs/technical/` |
| Bug-report bundle | Pack the log and XC trace with serials and addresses scrubbed; include a pcap only by explicit choice, preferably from Xombie. | S | `scripts/` |

### Could

| Item | What it does | Effort | Where |
| --- | --- | --- | --- |
| UPnP for UDP 3074 | Ask the home router to forward 3074, as Insignia's guide advises for hosting. | S | reuse `src/net/net_upnp.c` |
| Dashboard recompilation | Register and sign up from inside the launcher, without the real console. | L | a new title project |

## 6. Open questions and next steps

1. **Why the reply is not processed.** No decryption crypto follows the
   ticket. Check, in order: the reply reaches the game whole (pcap,
   fragments); `RECOMP_XC_TRACE` around it; the kernel's crypto exports
   against known answers; the BIOS-derived kernel keys, zeros today.
2. **Compare against a local Xombie**, step by step.
3. **Ask the Insignia team** (Discord) whether a recompiled client is
   welcome before wider testing.
4. **Other titles**: which carry a Live block, and where. OutRun 2 and
   MechAssault are next.
5. **One console, two clients**: can the real console and a recompiled build
   share one identity safely?

Sources: Tristan Miller, *Reversing the Original Xbox Live Protocols*
(DEF CON 30); feudalnate/Original-Xbox-LIVE-Account; xboxdevwiki.
