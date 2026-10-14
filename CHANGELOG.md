# Changelog

This project began as a fork of [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
and has since diverged a long way: a different renderer, a different memory model, a second
operating system, and a list of titles that run. The fork does not use release numbers yet, so
entries below are grouped by when the work landed. Pull request numbers are this repository's.

For where each title stands today, see the [README](README.md#title-status).

---

## October 2026

### Input scripts without a step limit (branch `feat/ts2-decomp`, 14 Oct)
- `RECOMP_INPUT_SEQ` no longer stops at 1024 steps: the array grows. A recorded play session
  (`RECOMP_INPUT_RECORD`) of six and a half minutes of TimeSplitters 2 was 1250 steps, and its
  replay stopped pressing partway through.
- `RECOMP_INPUT_SEQ_FILE=<path>` reads a script from a file, in the format `RECOMP_INPUT_RECORD`
  writes, for scripts too long to want in the environment. `RECOMP_INPUT_SEQ` wins when both are set.
- The start-up line names where the script came from and shows its first 80 characters, not all of it.

### macOS on Apple Silicon (#49, 7 Oct)
TimeSplitters 2 and BLiNX run on an M-series Mac at 60 fps with sound.

- **Memory.** arm64 macOS reserves the low 4 GB and uses 16 KB pages, so guest memory is now a 4 GB
  arena at a base offset (`XBOX_PTR` adds `g_xbox_mem_offset`; zero on Windows). Mirrors are extra
  mappings of one shared object, a 4 KB guest-protection table sits over the 16 KB host pages, and
  the main TIB moved to 0x4000.
- **Faults.** There is no SEH, so SIGBUS/SIGSEGV handlers run an AArch64 load/store emulator and
  route device ranges to the same fault path the other hosts use. Watchpoints work by emulating
  the store.
- **Threads.** A guest lock keeps one guest thread in lifted code at a time, with `RECOMP_BACKEDGE()`
  at every loop header so a spinner hands it over. It is scheduled as the console would: highest
  priority owns it, and preemption happens only at a back edge or blocking call. Measured cost is
  at most 2% of TimeSplitters 2's main thread. `KeQueryBasePriorityThread` had answered 0 for every
  thread on every host.
- **Lifter semantics, now explicit on every host.** Locked read-modify-write and `xchg` are real
  atomics (they previously did nothing anywhere), `cvtss2si`/`cvtsd2si` round and return
  0x80000000 on overflow, and division by zero raises the fault x86 would.
- **Host layer.** Window, input and sound come from SDL3. The audio device opens on its own thread,
  so a stuck `coreaudiod` costs only the sound. Per-user folders follow each OS's convention.
- **Tooling.** `scripts/make_macos_app.py` builds a local `.app`; `tools/macos/run_tests.sh` runs
  the unit tests; CI builds and tests on macOS.

### BLiNX: the Time Sweeper (#47, 7 Oct)
Reaches its first level, drawn, with its movies playing.

- Its image links 38.7 MB of model and map data as 42 demand-loaded sections. Swept as code they
  made 10,700 phantom functions. `config/sections/<TITLEID>.json` now names a title's data
  sections, which cut the lift from 17,225 functions to 9,124 and disassembly from 612 s to 59 s.
- For an XBE with more than 1 MB of non-preload sections the layout maps extra address space.
- Three renderer rules: a declaration on one non-zero stream, a draw sampling its own render
  target, and a stage with no texture.

### Need for Speed: Underground 2 and WWE Raw 2 in game (#42, #43, #45, #46)
Two titles reach gameplay. Fixes that came out of them are in the shared lifter, kernel and HLE.

### Lifter correctness (#37, #38, #39, #30)
Flag joins: a join edge that needs the carry flag now has it declared, and flags are carried
across joins instead of being recomputed. Lifter and kernel fixes from upstream's 30 September
batch were merged in.

### Smaller changes
- A vertex program's fog output is treated as a fog coordinate (#41).
- `recomp_hle.c` is written only when its content changes, so a rebuild no longer recompiles it (#40).

### Outrun 2, and frame pacing (#28, #29, #31, #32, #34)
- **Frame interpolation** (`RECOMP_FRAME_INTERP=2`): the newest frame's host calls are redrawn in
  the flip gate's wait with their matrix constants blended toward the previous frame, so a title
  that runs at 60 is shown at 120. See [frame-interpolation.md](docs/technical/frame-interpolation.md).
- **Variable refresh rate** support in the flip gate.
- Outrun 2 was taken to about 40 fps and then through a fidelity pass, which drove much of the
  renderer performance work.

### Mortal Kombat: Deadly Alliance (#25, #26)
Reaches its Arcade fights. Its engine runs game logic as cooperative tasks that switch stacks by
hand and `jmp` into the middle of functions, which lifted C cannot express, so each task runs on a
host fiber ([mkda-coroutine.md](docs/technical/mkda-coroutine.md)). Along the way: a guest
`longjmp` to a buffer armed on another stack is detected and handed to a title handler, vertex
shaders created before `CreateDevice` are replayed to the shadow device, and fixed-function draws
described by a declaration are drawn through an equivalent FVF. `CopyRects` now copies what was
asked for, unscaled.

### Halo and Hunter: The Reckoning (#35)
Two titles on XDKs older than 4034. See [Contributors](CONTRIBUTORS.md).

---

## Late September 2026

### Vulkan renderer (#19), SDL3 (#20)
The renderer is no longer Windows-only. `src/d3d` gained an RHI, and `rhi_vulkan.c` is the renderer
everywhere but Windows (`RECOMP_D3D8_BACKEND=vulkan` selects it there). The HLSL generators stay as
they were; DXC compiles them to SPIR-V at run time with `-HV 2018`. `d3d8_gl.c` was deleted. Two
fixes that also change what Windows shows: the display resolve sampled half a pixel off, so every
presented frame had been slightly blurred, and a `Clear` with rectangles cleared the whole target.
Plan and decisions: [vulkan-backend.md](docs/technical/vulkan-backend.md).

### Widescreen and the launcher (#16, #22, #23, #24, #27)
A widescreen toggle, a UI pass that pins each 2D draw to the screen edge it is nearer, and a
fix so clicking an arrow in the settings launcher moves the way it points. See
[widescreen-and-resolution.md](docs/technical/widescreen-and-resolution.md).

### More titles
- **Marvel vs Capcom 2** compatibility (#13) and **Dino Crisis 3** in game (#17).
- **Outrun 2** and **Otogi** in game, movies play, and `lahf` and call-table fixes (#12).
- **TimeSplitters: Future Perfect** reaches its front end. Five faults sat on top of each other,
  each needing its own measurement; they are listed in
  [shadow-mode.md](docs/technical/shadow-mode.md).

### Run-time behaviour
- The executable runs a game by itself: it finds the game in a `game` folder beside it, then under
  `games/<title>/`, then at `RECOMP_GAME_DIR`. The vblank, the audio hardware and the shadow
  renderer are on unless turned off.
- **F9** shows the frame rate, **F10** steps the frame cap, **F11** captures the frame on screen.
- Input is bound rather than hard-coded: `input_bindings.json`, and `py -3 -m tools.input_ui`.
- `RECOMP_SAVE_DIR` moves the saves, and every run says where (#21).
- Process exits are traced with `RECOMP_EXIT_TRACE`.
- The projection register is expanded correctly and a title can override which registers hold its
  matrices (#18).
- Audio de-sync turned out to be dropped sound rather than a clock. A refused chunk is now retried,
  chunks are whole, and a stream's packets complete when the host has played them.

---

## Mid September 2026

### TimeSplitters 2 plays
The second title. It was the first proof of how much the toolkit treated as universal: the retail
disc check, GPU time fence offsets, the DSP doorbell, the APU's physical view and two template gaps
were all wrong for a second XDK ([second-title-bringup.md](docs/technical/second-title-bringup.md)).
The 4721 shader paths are replaced, and a switch table the disassembler never measured is now
re-synced before every function rebuild. The level runs at about 80 fps uncapped, and a flip gate
in adaptive mode is on by default. The performance plan in
[ts2-performance-plan.md](docs/technical/ts2-performance-plan.md) took a level frame from 12.4 ms
to 5.0 ms.

### Shadow mode
Each replaced XDK function runs the title's own body first and, with `RECOMP_HLE_D3D8=shadow`,
repeats the call on a host device in a second window. Burnout 2 was the first title drawn this
way: logos, menus, loading screens, HUD and race, with its own vertex programs and register
combiners. Written up in [shadow-mode.md](docs/technical/shadow-mode.md).

### Frame capture and replay
`src/hle/d3d8_capture.h` and `src/replay` record one frame's host calls, and play them back with
no game running, so a frame can be drawn by two backends and compared. This is the bring-up loop
for a new backend.

### Cube maps and render targets
Rendering into cube maps and sampling what was rendered; a cube face is 128-byte aligned.

### Upstream v0.9.0 merged (14 Sep)
Thirty commits from upstream, including the 371-ordinal kernel routing, were merged and four
conflicts resolved. Upstream's version history is summarised [below](#before-the-fork).

---

## Early September 2026

The groundwork the first title needed.

- **Decoder and disassembler:** indexed jump tables lifted even when the low slots are never used,
  tables indexed backwards from their dispatch, conditional jumps into a function given an alias
  entry, and the backward table scan stopped at its own dispatch jump.
- **NV2A and APU:** vertex-program encoding derived and checked against a reference
  ([nv2a-vertex-program-encoding.md](docs/technical/nv2a-vertex-program-encoding.md)), decode and
  CPU execution moved out of Direct3D and into `src/kernel`, the vblank interrupt chain made to
  work, the DSP command write trapped, and three pieces of audio that were each built and
  unreachable connected.
- **Kernel:** `KfRaiseIrql`/`KfLowerIrql` are fastcall, the EEPROM factory block is answered, and
  `XC_AUDIO` no longer claims mono with an encoder that does not exist.
- **Diagnostics:** scripts that find where a stalled title stopped, name a watchdog dump's call
  chain, and say whether a title was still finding new code.
- **Seeds belong to a title**, not to the toolkit (`config/`).
- **CI** builds and tests on Linux and Windows on every push.
- **Docs:** [INSTRUCTIONS.md](INSTRUCTIONS.md), a seven-step walkthrough.

---

## Before the fork

The project this one grew from released v0.1.0 *First Light* (March 2026) through v0.9.0
*Quietly Wrong* (September 2026): the parser, disassembler, function identifier, lifter and first
runtime, then the fixed-function and programmable D3D8 paths, and finally a large batch of
contributed correctness fixes and the routing of all 371 kernel ordinals. Credit for that work is
in [CONTRIBUTORS.md](CONTRIBUTORS.md) and [the acknowledgements](README.md#acknowledgements), and
the history itself is preserved in this repository's `git log`.
