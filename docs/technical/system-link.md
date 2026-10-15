# System Link: the network card

System Link is plain Ethernet between consoles, and the network stack that
speaks it (XNet) is linked into every title and drives the card's registers
itself. So the card is where this runtime steps in: below it a frame is a
byte array, above it is the title's own lifted code. The proposal and the
plan, including XLink Kai and Insignia, are in
`docs/System Link over the internet.md`.

This page records what exists and what was measured. TimeSplitters 2 is the
reference title.

**Status (2 Oct 2026):** two copies of TimeSplitters 2 on one PC find each
other's System Link game, join, and play a Deathmatch, also with 100 ms
added each way, and with 50 ms and 5% loss. Next is playing against xemu
(which needs the player's own `XboxLANKey`, below) and XLink Kai.

**Status (11 Oct 2026):** hosted sessions for up to 16 consoles, chosen in the
launcher's System Link tab ("Hosting a session", below). The switch is
checked by `tests/syslink_hub` on 127.0.0.1 with no game; **no game has been
played through it yet**, on one PC or several, and the router forward has not
been tried against a real router.

## What exists

| Piece | Where | Switch |
| --- | --- | --- |
| The network card | `src/kernel/xbox_nic.c`, a fault range it registers itself (`recomp_fault_add_range`, routed by `xbox_fault_route.c`); interrupt from the kernel's timer thread | `RECOMP_SYSLINK=host`, `join` or `udp` (`syslink` in the settings file) |
| A hosted session: the host is a learning switch for up to 16 consoles | `src/kernel/xbox_net_udp.c`; checked by `tests/syslink_hub` | `syslink_host`, `syslink_port`, `syslink_code` (`RECOMP_SYSLINK_HOST`, `_PORT`, `_CODE`) |
| The router asked to forward the host's port (UPnP), and the addresses to give out | `src/net/net_upnp.c`, over `third_party/miniupnp` (miniupnpc 2.3.3) | `syslink_upnp` (`RECOMP_SYSLINK_UPNP`), on by default |
| The launcher's System Link tab | `src/launcher/launcher.c` | writes the five `syslink*` settings |
| The internet through a router in the runtime, for Xbox Live (Insignia): DHCP, ARP, UDP out through this PC (the console's own port kept when free), fragments both ways, TCP refused for now | `src/kernel/xbox_net_nat.c`; checked by `tests/net_nat` | `RECOMP_SYSLINK=nat` (`syslink = nat`), `RECOMP_NET_DNS` (`net_dns`, default `46.101.64.175,1.1.1.1`), `RECOMP_NET_LOG=1` |
| A UDP tunnel in xemu's format | `src/kernel/xbox_net_udp.c` | `RECOMP_SYSLINK=udp`, `RECOMP_SYSLINK_REMOTE=host:port`, `RECOMP_SYSLINK_LOCAL=port`, `RECOMP_SYSLINK_DELAY_MS`, `RECOMP_SYSLINK_LOSS` |
| Real Diffie-Hellman and triple DES in the kernel | `src/kernel/xbox_crypto_soft.c`, behind `XcModExp`, `XcKeyTable`, `XcBlockCrypt(CBC)`, `XcDESKeyParity`; checked by `tests/crypto_soft` | |
| `XboxLANKey` from the player | `xbox_NetLanKey` in `xbox_nic.c` | `%APPDATA%\xboxrecomp\keys.ini` `lan_key = ...`, or `RECOMP_XBOX_LAN_KEY` |
| A unique Ethernet address per save folder | `src/kernel/xbox_nic.c`, answered through `ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR)` in `kernel_xbox.c` | `RECOMP_SYSLINK_MAC=00:50:F2:12:34:56` overrides it |
| A register log of the card at 0xFEF00000 | `src/kernel/xbox_nic.c` | `RECOMP_NIC_TRACE=1`, `RECOMP_NIC_TRACE_BUDGET=<lines>` |
| A line per frame carried | `xbox_nic.c` | `RECOMP_SYSLINK_FRAMES=<n>` |

Everything is off by default: without `RECOMP_SYSLINK` or `RECOMP_NIC_TRACE`
the register page is the plain memory it always was, and a title on no
network shows what it should: TimeSplitters 2 searches, then says "No System
Link Games Found".

### Hosting a session

One player hosts, everyone else joins; the host's copy is the network switch
the session runs through. In the launcher's System Link tab:

- **Host a session.** The tab shows the address to give out: this PC's on its
  own network, and the router's internet address once the router answers
  (Ctrl+C or a click copies it). With **Open the router** on (the default),
  the game asks the router over UPnP to forward the UDP port to this PC while
  it hosts, with a one-hour lease renewed every half hour.
- **Join a session.** Type or paste the host's address (`:port` on the end if
  theirs is not 27610). Nothing to open on a joining PC's router: the host
  answers to the address the join came from, and a ping every two seconds
  keeps that way back open.
- **Join code**, optional on both sides: a host with one lets in only consoles
  that say the same word (not case-sensitive).

Then everyone starts the game and uses its own System Link menu as on a
console: any console in the session can host the game there, not only the
one hosting the session. If the session's host quits, the session ends.

The same from the environment, which wins over the settings file:

```
host: RECOMP_SYSLINK=host [RECOMP_SYSLINK_PORT=27610] [RECOMP_SYSLINK_CODE=word] [RECOMP_SYSLINK_UPNP=0]
join: RECOMP_SYSLINK=join RECOMP_SYSLINK_HOST=81.2.3.4[:port] [RECOMP_SYSLINK_CODE=word]
```

What the log says, in order, on the host:

```
[NET] hosting System Link on UDP port 27610, up to 16 consoles; on this network friends join 192.168.1.20:27610
[NET] the router forwards UDP port 27610 here: friends on the internet join 81.2.3.4:27610
[NET] a console joined from 86.5.6.7:51234: 2 in the session
```

and on a joining copy `[NET] joined the session at ...`, then every 30 s its
round trip to the host. A refusal names its reason (wrong code, full, a
different version, or the same console identity as one already in -- two
copies sharing a save folder). When the router does not answer, or answers
with an internet address that is itself private (a second router, or the
ISP's shared address), the log and the launcher say friends outside need the
port forwarded by hand or a virtual network (ZeroTier, Tailscale).

The session's own messages are four bytes ('X' 'L', version, type) ahead of
a hello (join code, Ethernet address), a welcome (how many are in), a refusal
(why), a frame, a ping and its echo, or a goodbye. Until a console is
welcomed, nothing it sends reaches anyone. The switch learns each console's
Ethernet address from its hello and from what it sends; a broadcast goes to
everyone but its sender, a frame for one console to that console, and one for
an address not seen yet to everyone but its sender, as a learning switch
floods. It forwards on its receive thread, so a frame between two joined
consoles never waits for the host's game.

Limits: IPv4 only; a full 1514-byte frame plus the 4-byte header is past the
1472 bytes of a 1500-byte internet packet and will be fragmented (no title
has been seen to send one, see "Measured" below).

**macOS (15 Oct 2026).** The card runs on POSIX too: the same model, reached
through the signal handler and the arm64 decoder (`recomp_fault_emulate`),
with guest memory reached through the arena's backdoor so the handler can
never fault twice. Checked on Apple Silicon with Rainbow Six 3: two copies on
one Mac, one hosting and one joining 127.0.0.1, exchange XNet's frames both
ways (1 ms round trip), and `tests/syslink_hub` passes. No match has been
played across them yet, and no Mac has played a Windows PC yet.

**NAT mode (15 Oct 2026).** `syslink = nat` cables the card to the
internet instead of to other consoles. Rainbow Six 3 on macOS takes its
address 17 ms after its first DHCPDISCOVER (10.0.2.15, DNS Insignia's then
1.1.1.1), probes it by ARP, hears nothing, and carries on. With nothing
answering it retried for half a minute or more. Its Xbox Live menu has not
been tried through it yet.

With the card on, `PhyGetLinkState` reports a cable whatever
`RECOMP_ETHERNET_LINK` says (`xbox_NicCardOn`), so a title that turns the
link off for the no-card case (Rainbow Six 3) still finds it.

Two copies on one PC, hosted: one hosts, the other joins 127.0.0.1, each
with its own save folder (its own Ethernet address), and the router left
alone since nobody outside is joining:

```
copy A: RECOMP_SAVE_DIR=<a> RECOMP_SYSLINK=host RECOMP_SYSLINK_UPNP=0
copy B: RECOMP_SAVE_DIR=<b> RECOMP_SYSLINK=join RECOMP_SYSLINK_HOST=127.0.0.1
```

Three or more on one PC is the same with more copies like B, each with its
own save folder.

### Two copies on one PC

Give each its own save folder (so its own Ethernet address) and point their
tunnels at each other:

```
copy A: RECOMP_SAVE_DIR=<a> RECOMP_SYSLINK=udp RECOMP_SYSLINK_LOCAL=9001 RECOMP_SYSLINK_REMOTE=127.0.0.1:9002
copy B: RECOMP_SAVE_DIR=<b> RECOMP_SYSLINK=udp RECOMP_SYSLINK_LOCAL=9002 RECOMP_SYSLINK_REMOTE=127.0.0.1:9001
```

For a scripted pair: host with the Join Game sequence below but Start Game
instead of Join, then Deathmatch, map, options, character and the game name
(six more A presses, about 6 s apart, the last landing in the lobby), and a
final A to start once the joiner is in; the joiner presses Join Game after
the host's lobby is up, then A for the server, A for the character.

### The Ethernet address

Every install used to be 00:50:F2:00:00:01, and two consoles with one
address cannot share a System Link game. Now the first time a title asks, an
address is chosen at random under Microsoft's 00:50:F2 prefix and written to
`console.ini` in the save folder, beside the partition images:

```
[NET] Ethernet address 00:50:F2:8E:C2:08 (new, saved to console.ini)
```

The save folder because that is the console's disk. On a console the
identity (the EEPROM) and the disk go together, and Insignia will need them
to match. It also means a scripted run under `RECOMP_SAVE_DIR` gets an
identity of its own and never writes the player's.

A title asks only when its network code starts, not at boot. TimeSplitters 2
asks when System Link's Join Game or the hosting path starts XNet.

### The register log

```
[NIC] write +0x0A8 MacAddrA           = 0x8EF25000 (4) sub_0020FD1E+0x131D  [first]
[NIC] write +0x144 TxRxControl        = 0x00000001 (4) sub_0020FACA+0x138
[NIC]   ... and 3411 more times
```

A run of identical accesses prints once with a count. Past the line budget,
only the first access to each register still prints. Every five seconds the
timer thread prints a summary of each register touched, and for the first
three summaries after XNet sets them up, the descriptor rings and the start
of each queued frame. An instruction the decoder does not know stops the log
and leaves the page as plain memory, rather than stopping the title.

The register names are Linux forcedeth's for the same nForce MAC, there as a
reading aid.

## Getting TimeSplitters 2 to System Link in a scripted run

```
RECOMP_INPUT_SEQ=12000:start,16000:start,20000:start,24000:start,46000:a,56000:down:100,60000:a,66000:down:100,68000:down:100,71000:a,76000:down:100,79000:a
```

That is: the start screens, profile storage (the "saving profile" screen
moves on by itself, so no second A), Arcade, System Link, Join Game. Two
things that cost runs. A `down` held for 300 ms auto-repeats and moves the
cursor two rows, so hold it 100 ms. And the script's clock starts at the
title's first pad read, not at process start.

TimeSplitters 2 writes its profiles through `\Device\Harddisk0\Partition1\`,
which maps to the game folder, so `RECOMP_SAVE_DIR` does not isolate them.
Move `games/Time Splitters 2/UDATA/4553000a/<12 hex digits>/` aside before
scripted runs and put them back after.

### A discovery gap on the way

Selecting System Link made an indirect call to 0x0009D530, which the
disassembler had never found. The skipped call returned 0, the title went
into its error path (`[INT3]` after a call it treats as never returning),
and it crashed writing through 0xFFCFFFF0. Seeded in
`config/seeds/4553000A.json` with `tools.seed_from_log`, re-run from the
disasm stage, and the menu opens. (`seed_from_log` rewrote the seed file with
CRLF line endings; it was put back to LF.)

## What XNet does, measured (TimeSplitters 2, XDK 4721)

From `RECOMP_NIC_TRACE=1` through Join Game. Nothing touches the card before
that point. The driver is five functions at 0x0020F578 to 0x0020FD1E, and its
interrupt routine (0x0020F742) connects on bus level 4, as on the console.
(The first logs said vector 0: that was a kernel bug, below.)

**Reset.** `LinkSpeed`, `ReceiverControl`, `TransmitterControl` cleared, then
`ReceiverStatus` and `TransmitterStatus` read for bit 0 (busy, clear on plain
memory, so fine). Then `TxRxControl = 4` (reset) and a wait of up to 500
reads for **bit 3 of `TxRxControl`**, forcedeth's IDLE. Plain memory never
sets it, so the wait timed out; the card reports it.

**Set-up.** In order: `MIIMask`, `IrqMask = 0`, `+0x200`, `UnknownSetupReg6`,
`TransmitPoll`, `LinkSpeed`; status registers read and written back
(write-1-to-clear acknowledgements); then:

| Register | Value | Meaning |
| --- | --- | --- |
| `MacAddrA` / `MacAddrB` | `0x8EF25000` / `0x000008C2` | 00:50:F2:8E:C2:08, the address `console.ini` chose |
| `MulticastAddrA/B`, `MulticastMaskA/B` | all ones | receive broadcasts |
| `OffloadConfig` | `0x5EE` | 1518, the largest frame |
| `PacketFilterFlags` | `0x007F0020` | |
| `Misc1` | `0x003B0F3E` | |
| `TxRingPhysAddr` | `0x03684000` | send descriptors |
| `RxRingPhysAddr` | `0x03684800` | receive descriptors |
| `RingSizes` | `0x0007000A` | 8 receive, 11 send, packed (rx-1) << 16 \| (tx-1) |
| `AdapterControl` | `0x01040000`, then `0x01140000` | |
| `MIISpeed` | `0x105` | |
| `LinkSpeed` | `0x00010064` | |
| `TransmitterControl`, `ReceiverControl` | `1` | start |
| `TxRxControl` | `3` | |
| `MIIMask` | `8` | link change |
| `IrqMask` | `0x5F` | |

**Sending.** XNet writes `TxRxControl = 1` (kick) every time it queues a
frame, from `sub_0020F96F`, `sub_0020FA9C` and `sub_0020FACA`. With no card
behind the registers nothing ever completed, the ring stayed full (7 of 11
queued) and XNet kicked about 500 times a second. Its completion path
(`sub_0020F8A2`) checks only that bit 15 of a descriptor has been cleared.

**Receiving** (`sub_0020F74E`, from the DPC `sub_0020FCB6`) reads a
descriptor's length as the frame's own length -- it subtracts one only for an
error-flag combination -- skips one without bit 0 (valid), re-arms each one
itself with `0x800007FD`, and filters on its own address or broadcast in
software. The DPC acknowledges `MIIStatus` and `IrqStatus` by writing back
what it read, and restarts the receiver (`TxRxControl = 2`) on "no buffer".

**Descriptors** are 8 bytes: a 32-bit buffer physical address, a 16-bit
length minus one, and 16 bits of flags.

```
send    [0] buffer 0x011FE8F2 length 0x0041 flags 0x8001   66-byte frame, valid | last
receive [0] buffer 0x03685002 length 0x07FD flags 0x8000   2 KB buffer, owned by the card
```

**The frames** are XNet's search for a host: Ethernet broadcast from this
console's address, IPv4 from 0.0.0.1 to 255.255.255.255, **UDP port 3074 to
3074**. There is no DHCP: on System Link XNet sends straight away, so there is
no address wait to answer.

```
dst FF:FF:FF:FF:FF:FF src 00:50:F2:57:AD:18 type 0800: 45 00 00 34 ... 40 11 ... 00 00 00 01 FF FF FF FF 0C 02 0C 02 ...
```

## Physical addresses: two storages

In this runtime, low RAM and the contiguous window at 0x80000000 are
**separate storage** (`xbox_memory_layout.c`, kept apart on purpose so pinned
pools do not land on the XBE image), and `MmGetPhysicalAddress` answers a low
VA with itself and a window VA with VA - 0x80000000. So a physical address
alone does not say which storage it came from.

XNet uses both. The rings come from `MmAllocateContiguousMemory` and read
correctly through the window. The **frame buffers are in low RAM**
(0x011FE8F2 is XNet's pool): read through the window they are another
allocator's `mem_mark` fill, read through low RAM they are the frames above.
The APU reads every physical address through the window, which is right for
its buffers and would be wrong here.

"Inside a live contiguous block means the window" does not work: the window
held another allocator's data at the same physical address as the frame
buffer, so both storages are in use there. What does work is asking the one
place that knows: `MmGetPhysicalAddress` now notes, per 4 KB page, which
storage it answered for, and `xbox_PhysToGuest` resolves through that (the
window, as before, for a page never handed out). Over a 150 s match every
send buffer resolved to low RAM, as it should.

## What it took, beyond the card

**A kernel bug that moved the interrupt.** `KeInitializeDpc` cleared 32 bytes,
but an Xbox KDPC is 0x1C. XNet keeps its interrupt vector in the field right
after its DPC, so the vector `HalGetInterruptVector(4)` returned was wiped
before `KeInitializeInterrupt` read it, and the ISR connected on vector 0.
Any title with a field after an embedded DPC lost it the same way.

**Real crypto.** Two copies found each other's game, then the joiner sent its
key-exchange packet, the host answered, and the host rejected every packet
after that. XNet's key exchange is Diffie-Hellman (`XcModExp`) and its
traffic triple DES in CBC (`XcKeyTable`, `XcBlockCryptCBC`); all were stubs,
so each side computed a different shared secret. They are the standard
algorithms now, checked against FIPS 46, FIPS 81, SP 800-67 and Python's
`pow()` (`tests/crypto_soft`).

**`XboxLANKey`.** XNet derives a title's System Link keys as
HMAC(`XboxLANKey`, 0 || the certificate's LAN key) ([xboxdevwiki][sl]).
`XboxLANKey` is one fixed key in every retail kernel and Microsoft's, so it is
not in this repository. Two recompiled builds agree on zeros, which is why
the pair works; playing xemu or a console needs the player's own key from
their own BIOS in `keys.ini`. Unverified until the xemu check.

[sl]: https://xboxdevwiki.net/System_Link

## Measured: two copies on one PC

| Run | Join | Match | Frames (host out / in) |
| --- | --- | --- | --- |
| No added delay, 150 s in the level | yes | held to the end | 1654 / 1506 |
| 100 ms added each way | yes | held to the end | 1336 / 1233 |
| 50 ms each way, 5% loss | yes | held to the end | 1324 / 1157, 130 dropped in all |

In a match TimeSplitters 2 sends about 11 frames a second each way. Frame
sizes were logged only through the join (the largest there was 298 bytes);
whether match traffic ever approaches 1514 bytes, and so the 1500-byte
internet packet, is still to be measured. Received frames reach the guest on the next timer
tick, which can add up to about 16 ms; worth measuring against a real
round trip before changing the kernel's pacing for it.

Whether a match *feels* playable at a given delay is a person's judgement,
not something these runs show.
