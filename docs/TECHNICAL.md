# Technical notes

How the port works internally, for people who want to hack on it. The player-facing
documentation is the [README](../README.md).

## Pipeline

```
your disc image (.cue/.bin)
   │  tools/iso.py        ISO9660 reader: SLUS_011.15 (main executable), A_FILE.HDT/A_FILE.BIN
   ▼                      (archive holding the code overlays); verified by SHA-256 in build.py
extract/
   │  tools/recomp.py     static recompiler: MIPS R3000A machine code -> C
   ▼
generated/               ~3,800 C functions, ~380k lines (main executable + 13 overlays)
   │  tools/compile.py    clang (llvm-mingw) on Windows, cc on Linux, zig cc for Linux cross builds
   ▼
build/<config>/HarvestMoonBTN(.exe)   runtime/ + generated/ linked into one native program
   │  tools/package.py
   ▼
dist/HarvestMoonBTN-<OS>-x64/          exe + SDL2 + hmbtn.ini + README (+ HarvestMoonBTN.pak)
```

`build.py` runs all of it, and only redoes the steps whose inputs changed (`generated/.stamp`
holds a hash of the game files and the recompiler sources).

## The recompiler (`tools/recomp.py`)

* **Modules.** The main executable (`SLUS_011.15`, loaded at 0x80010000) and the 13 code
  overlays listed in `tools/overlays.json` (file index inside `A_FILE.BIN`, load address,
  code range). Most overlays share the load address 0x800B7E20, so the same address can hold
  different code at different times.
* **Function discovery** is a fixpoint over: the entry point, `jal` targets, `lui`/`addiu`
  code-address constants (including ones built in branch delay slots), data words that point
  at code, a sweep after each `jr ra`, and jump-table targets. Starts that are certain (`jal`
  targets, code constants) are "strong" and end the function before them; starts that are
  only likely (data pointers, the post-`jr ra` sweep, which must decode as a valid
  instruction) are "weak": they get their own C function but don't split a preceding one.
  `tools/extra_entries.json` adds entry points that cannot be found statically (the libetc
  `setjmp` return used by HookEntryInt).
* **Code generation.** Each MIPS function becomes `void f_<address>(CPU *c)`; overlay
  functions are prefixed with the overlay name. Registers live in the `CPU` struct, memory
  accesses use the inline helpers in `runtime/psx_recomp.h`, branches become `goto`s (the
  delay-slot instruction is emitted before the jump, after the condition is evaluated),
  recovered jump tables become `switch` statements, and indirect calls (`jalr`, unresolved
  `jr`) go through the runtime dispatcher `call_addr()`. Unaligned `lwl/lwr/swl/swr` and
  `div` by zero follow the R3000A rules; overflow traps are not modelled (`add` behaves like
  `addu`).
* **Timing.** Every block adds its cost to `c->cycles` (loads ~5 cycles, mult 7, div 36,
  GTE commands with their documented latencies). The game's VSync timeout logic and its
  CD/SPU/DMA waits depend on time advancing realistically, so this is not optional.
* **Overlays.** Each function stores its first two instruction words ("check words"). When
  the dispatcher resolves an address in an overlay region, it checks which overlay is
  actually loaded in RAM and picks the matching translation. The same check at boot rejects
  disc images that don't match the recompiled code.

A synthetic example of what the translation looks like (not code from the game):

```
MIPS                                   generated C
---------------------------------      ----------------------------------------------
loop: lw    t0, 0(a0)                  L_loop:
      addiu a0, a0, 4                    R[8] = MEM_R32(R[4]);
      addiu a1, a1, -1                   R[4] = R[4] + 4u;
      bne   a1, zero, loop               R[5] = R[5] - 1u;
      addu  v0, v0, t0   (delay slot)    c->cycles += 9;
      jr    ra                           { int br_ = R[5] != 0;
                                           R[2] = R[2] + R[8];          /* delay slot */
                                           if (br_) { EVCHK(); goto L_loop; } }
                                         return;
```

`EVCHK()` lets the scheduler run hardware events (timers, VBlank, CD, SPU) inside long loops.

## The runtime (`runtime/`)

| File | What it reimplements |
|---|---|
| `cpu.c` | Dispatcher for indirect calls (overlay residency check + check words), COP0, syscall/break, exceptions |
| `sched.c` | Cycle-driven scheduler, interrupt controller, the 3 root counters, NTSC video timing (263 lines × 3413 GPU clocks → 59.29 Hz) |
| `mem.c` | RAM, scratchpad, I/O map |
| `bios.c` | The BIOS kernel, natively: exception/IRQ chains, HookEntryInt/ReturnFromException (setjmp/longjmp), events, memory-card file system on a raw `.mcd` image, device table, libc helpers, `printf` → log. No Sony BIOS code is used |
| `gpu.c` | GP0/GP1 command processing and a software rasterizer reproducing the PS1 GPU (triangle/quad/line/sprite rules, texture windows, CLUTs, semi-transparency, dithering) |
| `gte.c` | Geometry Transformation Engine (COP2), bit-exact including flags |
| `spu.c` | Sound chip: 24 voices, ADPCM, ADSR, gaussian interpolation, reverb |
| `cdrom.c` | CD controller and drive: command/response/IRQ protocol, seek/read timing, XA-ADPCM decoding with the hardware's 37.8 → 44.1 kHz zigzag resampler |
| `cdpak.c` | Reader for the optional data pack (below) |
| `dma.c` | DMA channels (GPU linked lists, OTC, CD, SPU) |
| `sio.c`, `memcard.c` | SIO0 port: digital pad and memory-card protocol (the game's libpad/libmcx drivers talk to the port directly) |
| `platform_sdl.c` | Window, scaling, keyboard + game controllers, audio output, frame pacing, screenshots, headless test mode. `sdl_dyn.h` loads SDL2 at run time on Linux |
| `main.c` | Configuration (`hmbtn.ini`, command line), locating the game data, boot |

Things that cost time to find, in case you touch these areas:

* The game polls the controllers itself through libpad over SIO0 (not via the BIOS pad
  functions). A digital pad must acknowledge only command 0x42; acknowledging 0x43/0x45
  makes the driver think it is an analog pad.
* Pending interrupts must be dispatched again right after an exception handler returns:
  the libmcx memory-card driver depends on IRQ7 arriving during VBlank processing.
* libcd never sets the CD interrupt mask; the runtime starts with it set to 0x1F like the
  real BIOS leaves it.
* libapi `firstfile` walks the BIOS device table at [0x150]; libmcrd then waits for a HwCARD
  (0xF0000011, 0x0004) event.
* The PocketStation probe (command 0x81 'X') must receive no ACK from a normal card.
* Asynchronous card `read`/`write` (mode 0x8000) must return 0 ("accepted"); libmcrd retries
  while it is non-zero, which shows up as an endless "Saving...".
* The libetc VSync timeout uses a timer, so instruction costs must be realistic.

## The data pack (`HarvestMoonBTN.pak`, optional)

The game reads its data by CD sector address and streams XA audio interleaved with other
data, so the pack keeps the exact sector layout instead of loose files. `tools/makepak.py`
stores the 74,617 sectors before the `DUMMY.DAT` filler (the game never reads it; the
highest sector it seeks to is 73,505) as subheader + payload only: 58 MB instead of 214 MB.

```
"HMBTNPAK"  u32 version=1, total_sectors, stored_sectors, sectors_per_chunk=16, nchunks, 0
u32 offsets[nchunks + 1]      file offset of each zlib chunk
u32 crc32[nchunks]            CRC32 of the ORIGINAL raw 2352-byte sectors of each chunk
zlib chunks                   per sector: flags (bit0 form 2, bit1 form-2 EDC is zero),
                              4-byte subheader, 2048 (form 1) or 2324 (form 2) bytes
```

`runtime/cdpak.c` inflates a chunk (its own small inflate implementation), rebuilds each
sector bit-exactly (sync, BCD MSF header, subheader copy, EDC, ECC P/Q for form 1),
compares the chunk with its CRC32 the first time it is used, and caches recent chunks.
`HarvestMoonBTN --verify-data` checks every chunk (about 2 seconds).

Game data lookup order: the path on the command line or `disc =` in `hmbtn.ini` (`.pak`,
`.cue` or `.bin`), then `HarvestMoonBTN.pak` in the executable's folder, `data/` or the
parent folder, then the first `.cue`/`.bin` in the executable's folder or its `disc` folder
(and the repository's `disc/` folder for builds in `build/<config>/`).

## Testing and debugging

```
python tools/run_tests.py     unit tests for the GTE, GPU, SPU and memory-card file system
python build.py --debug       console build in build/debug with debug info
```

The executable has options for automated testing (all deterministic, so two runs with the
same options produce identical output):

| Option | |
|---|---|
| `--headless` | no window, no audio |
| `--unthrottled` | run as fast as possible |
| `--frames N` | quit after N frames |
| `--shots N --shotdir DIR` | screenshot every N frames |
| `--dumpram F` | write RAM + VRAM at frame F (repeatable) |
| `--wav FILE` | record the audio output |
| `--input "frame:BTN[:dur],start-end/period:BTN[:dur]"` | scripted button presses |
| `--fuzz SEED --fuzzstart F` | random button presses from frame F |
| `--calltest frame:addr:a0`, `--poke frame:addr:val` | call a game function / write memory at a frame |
| `--trace`, `--tracecalls` | log dispatcher activity / every call |
| `--log 0..4`, `--logfile F` | log level / log file |

`tools/sheet.py out.png a.bmp b.bmp ...` builds contact sheets from screenshots;
`tools/mdis.py` disassembles and cross-references the extracted executable.

During development the port was checked by running millions of frames of random input
without errors, by comparing debug and release builds and disc-image and data-pack runs
frame-by-frame (screenshots, RAM, VRAM, audio and memory card bit-identical), and by
playing through the title screen, a new game, the farm, town and a save/load round trip.
