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
| A user account on the disk | the first sectors of the HDD | not yet |

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

From [feudalnate/Original-Xbox-LIVE-Account](https://github.com/feudalnate/Original-Xbox-LIVE-Account):
an account is a 0x6C-byte record (XUID, gamertag, flags, passcode, Kerberos
domain and realm, a confounder and an HMAC-SHA1 / triple-DES signature with
keys in XOnline itself), up to eight of them near the start of the drive.
One EvoX backup of the first 512 KB (`disk.bin`) held a single record at
0x120C, the machine account (`SN.<serial>`, domain `xbox.com`, realm
`PASSPORT.NET`), and no user account.

## Next

1. Rainbow Six 3's Xbox Live menu through the router, with `RECOMP_NET_PCAP`:
   which names it asks for, and how far the Kerberos exchange gets.
2. The crypto exports checked against known answers, and the BIOS's keys.
3. A user account on the disk: copied from the console or xemu.
4. A local Xombie as a second server to check each step against.
