# Xbox Live (Insignia)

Original Xbox Live was shut down in 2010. [Insignia](https://insignia.live/)
replaces its servers for 199 titles, Rainbow Six 3 (`55530013`) among them.
This page records what the runtime does toward it and what was measured.
The plan is in `docs/System Link over the internet.md` (phase 8).

**Status (16 Oct 2026):** the card reaches the internet through a router in
the runtime, and a real console's identity is loaded from the player's own
backup. No title has signed in yet; Rainbow Six 3's Xbox Live menu has not
been tried.

## The client is already here

The Xbox Live client library (XOnline) is not in the dashboard or the kernel:
it is linked into every Live title, so the recompiler lifts it with the rest
of the game. What it needs from the runtime is what a console gives it:

| Piece | Where | State |
| --- | --- | --- |
| A network card and a cable | `xbox_nic.c`, `xbox_net_udp.c` | done, Windows and macOS |
| A router: DHCP, DNS servers, UDP out | `xbox_net_nat.c`, `syslink = nat` | done; `tests/net_nat` |
| The console's identity | `xbox_console_id.c` | done; `tests/console_id` |
| The kernel's crypto, exactly | `kernel_crypto.c`, `xbox_crypto_soft.c` | to check, export by export |
| The kernel's keys from the BIOS (`XboxSignatureKey`, `XePublicKeyData`, `XboxLANKey`) | `kernel_bridge.c` | zeros today |
| A user account on the disk | `Partition0.img`, from the backup | copied; not yet signed in with |

Rainbow Six 3 imports, besides the usual: `ExQueryNonVolatileSetting` (24),
`XboxHDKey` (323), `XboxSignatureKey` (325), `XboxLANKey` (353),
`XboxAlternateSignatureKeys` (354), `XePublicKeyData` (355), and the crypto
exports 335 to 349 (SHA-1, RC4, HMAC, `XcVerifyPKCS1Signature`, `XcModExp`,
DES and triple DES).

## What the protocol needs from the network

From Tristan Miller's DEF CON 30 talk (*Reversing the Original Xbox Live
Protocols*), the source of [Xombie](https://github.com/xombieonline/xombie):

- DNS for three targets only: MACS (machine account creation), the Kerberos
  AS and TGS, and a security gateway.
- Kerberos with Microsoft's pre-authentication extensions. MACS creates the
  machine account from the EEPROM's online key; its name is `SN.<serial>`.
- Everything after sign-in goes through the security gateway on UDP 3074, a
  VPN that carries the title's TCP and UDP inside it. A title has no TCP of
  its own on the wire, so the router carrying UDP alone is enough.

## The router (`syslink = nat`)

See the comment at the top of `xbox_net_nat.c`. The console is given
10.0.2.15 behind 10.0.2.2 (QEMU's layout), with Insignia's DNS
(46.101.64.175) first and 1.1.1.1 second. Rainbow Six 3 on macOS takes the
lease 17 ms after its first DHCPDISCOVER.

| Switch | |
| --- | --- |
| `RECOMP_SYSLINK=nat` (`syslink = nat`) | the card cabled to the internet |
| `RECOMP_NET_DNS` (`net_dns`) | the DNS servers handed out |
| `RECOMP_NET_LOG=1` | a line per UDP mapping and remote |
| `RECOMP_NET_PCAP=<file>` | every frame both ways, for Wireshark; it holds sign-in traffic, so keep it private |
| `RECOMP_NET_FRESH_PORTS=1` | never bind the console's own port on this PC |

The first 48 DNS names asked for are logged as `[NET] DNS: <name>`.

## The console's identity

See the comment at the top of `xbox_console_id.c`. The player backs up their
own console (EvoX, UnleashX and XBMC all do: `eeprom.bin` and `hddinfo.txt`)
into a folder `xboxbackup/` beside the game folder, or names it with
`console_dir`. From it come the serial, the Ethernet address and the online
key (`ExQueryNonVolatileSetting`), `XboxHDKey`, and the drive's serial and
model. The EEPROM's checksums are checked before any of it is used; nothing
is logged but where it came from and the address's maker prefix.

EvoX's `hddkey.bin` is the drive's lock password, not the HDD key. The key
is the `XboxHDKey` line of `hddinfo.txt`.

With an identity loaded, every copy run from that game folder is the same
console. Two copies in one System Link session need `console_dir = off` on
one of them.

## Accounts

XOnline keeps the console's accounts in the hard disk's config area, which
the runtime backs with `Partition0.img`. Rainbow Six 3 reads sector 8 and
sector 9 as its network starts. From a backup taken after signing up on
Insignia (EvoX's `disk.bin`, the drive's first 512 KB):

| Sector | What |
| --- | --- |
| 8 | an index the title updates every run |
| 9 | the machine account: named `SN.<serial>`, domain `insignia.live`, realm `passport.net` |
| 12 | the user account: the gamertag, domain `insignia.live`, realm `PASSPORT.NET` |

Each record starts 0x0C into its sector (name at +0x0C, Kerberos domain at
+0x24, realm at +0x38), the layout
[feudalnate/Original-Xbox-LIVE-Account](https://github.com/feudalnate/Original-Xbox-LIVE-Account)
describes. A backup from before signing up held a machine account from the
original Xbox Live (domain `xbox.com`) and no user account.

`xbox_console_import_accounts` copies sectors 8 to 15 from `disk.bin` onto
the disk image when the image has no account yet, and keeps the image's own
otherwise (`console_accounts = replace` takes the backup's, `off` leaves the
image alone). A title asking to sign up with no account on the disk reboots
to the dashboard (`HalReturnToFirmware`, launch reason 7) and, with none to
go to, comes back to itself: that is what an empty disk looks like.

## Signing in, measured (Rainbow Six 3, 16 Oct 2026)

Through the router, with the player's console identity and accounts:
DNS for `AS.XBOXLIVE.COM`, two AS exchanges (machine, then user) and a
TGS exchange with Insignia all succeed. Insignia's TGS-REP then carries
`XONLINE_E_LOGON_UPDATE_REQUIRED` (0x80151002): the title needs its
update. What it took, in order:

- A hard-drive copy's Live block undone (Rainbow Six 3's
  `scripts/live_unpatch.py`).
- The EEPROM's block reads answering the EEPROM (XNet reads all of it).
- `RtlNtStatusToDosError` mapping facility-7 statuses (XNet's "would
  block", 0x800703E5) on POSIX.
- XOnline's TGS-REQ body encoder seeded (reached only through its ASN.1
  encoder table).

## The online updater (`titles/xodash-update`)

On "update required", choosing OK makes the title call its title-update
routine, which reboots into `\Device\CdRom0;update.xbe` with launch data
naming the title (type 0, title ID at +4). The dashboard's own updater (the
5960 `xodash/update.xbe`, "Online Updater Application", title FFFE0000) is
recompiled in `titles/xodash-update`, its files in `games/xodash-update`
(the player's own, from their console's C: drive), 25 seeds found by run.

The runtime hands off between recompiled images by a launch map
(`launch_map` / `RECOMP_LAUNCH_MAP`, `kernel_bridge.c`): an Xbox path's
tail or a title ID to a host executable, optionally `|` its game folder.
The launch data page goes along in `RECOMP_LAUNCH_DATA`.

    RECOMP_LAUNCH_MAP="update.xbe=<xodash_update_recomp>|<games/xodash-update>;55530013=<rs3_recomp>|<rs3 game folder>"

Status: Rainbow Six 3 hands off to the recompiled updater with its launch
data; the updater boots, takes a DHCP lease, and then waits without
drawing a frame or signing in. That wait is the next thing to find.

## Next

1. Rainbow Six 3's Xbox Live menu through the router, with `RECOMP_NET_PCAP`:
   which names it asks for, and how far the Kerberos exchange gets.
2. The crypto exports checked against known answers, and the BIOS's keys.
3. Sign-in with the account copied from the console.
4. A local Xombie as a second server to check each step against.
