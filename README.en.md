# px68k ARCL — Windows X68000 Emulator / MCP Server

[日本語版](README.md)

This project provides a Windows frontend for the Sharp X68000 emulator core based on [px68k-libretro](https://github.com/libretro/px68k-libretro). In addition to normal interactive use through an SDL2 GUI, it can run as a stdio server for the [Model Context Protocol (MCP)](https://modelcontextprotocol.io/). An MCP client can capture the display, send input, interact with the Human68k console, inspect debugging information, and manage save states.

This repository contains no BIOS, CG-ROM, Human68k, game software, or other disk images. Use only files that you have obtained and are entitled to use.

## Requirements

- Windows 10 or Windows 11, 64-bit
- An MSYS2 `MINGW64` environment for source builds
- BIOS and CG-ROM dumps from an X68000 system
- A bootable disk image in `.XDF`, `.HDF`, `.D88`, or `.DIM` format

The frontend and the documented build procedure target Windows / MinGW-w64 only.

## Required files

### BIOS and CG-ROM

The X68000 BIOS and CG-ROM are copyrighted Sharp firmware. Obtain the following files from hardware or another source that you are entitled to use, then place them here:

```text
px68k/
  system/
    keropi/
      iplrom.dat
      cgrom.dat
```

The directory is not tracked by Git. Create it after a fresh clone if necessary:

```powershell
New-Item -ItemType Directory -Force px68k\system\keropi
```

Do not commit or redistribute BIOS, CG-ROM, Human68k system disks, or game disks with this project.

### Disk image

Store the image you intend to boot anywhere on the host. The examples below use `C:\X68000\HUMAN302.XDF`. Quote or otherwise pass paths containing spaces as individual command arguments.

## Build

1. Install [MSYS2](https://www.msys2.org/).
2. Open the **MSYS2 MINGW64** shell and verify that `MSYSTEM=MINGW64`.
3. Install build dependencies:

```bash
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-cmake
```

4. At the repository root, configure and build:

```bash
cmake -S windows -B windows/build -G "MinGW Makefiles"
cmake --build windows/build -j
```

The executable is written to `windows/build/arcl_windows_x68k.exe`. SDL2 and zlib are configured for static linking, so a normal build does not require separately distributing SDL2 or zlib DLLs. To rebuild from a clean CMake configuration, remove `windows/build/` and run the configure command again.

## Run in interactive mode

The following command opens the SDL2 window and boots the selected disk image:

```powershell
windows\build\arcl_windows_x68k.exe --system-dir px68k\system C:\X68000\HUMAN302.XDF
```

Interactive mode processes GUI events, rendering, and emulation on one thread. Its speed depends on the host and is not limited to original hardware speed.

| Key | Action |
|---|---|
| `Esc` | Exit |
| `F5` | Pause / resume |
| `F1` | Toggle frame/state information in the window title |
| `F2` | Save the current display as a PNG image |

CPU clock and RAM size can be selected at startup:

```powershell
windows\build\arcl_windows_x68k.exe --system-dir px68k\system --clock 25 --ram 8 C:\X68000\HUMAN302.XDF
```

- `--clock`: `10`, `16`, `25`, `33`, `66`, or `100` MHz (default: `10`)
- `--ram`: `1` through `12` MB (default: `2`)

## Run in MCP mode

In MCP mode, the executable processes newline-delimited JSON-RPC 2.0 through standard input and output. Standard output is reserved for MCP messages, so start it as a stdio server from an MCP client rather than using it directly in a terminal.

```powershell
windows\build\arcl_windows_x68k.exe --mcp --mcp-layers all --system-dir px68k\system C:\X68000\HUMAN302.XDF
```

The SDL2 window remains available in MCP mode. A person can observe the agent's actions and use `F5` to pause or resume execution. The emulator starts paused; it advances only after the client calls `arcl_run` or `arcl_resume`.

### MCP client configuration

`.mcp.json.example` and `.codex/config.toml.example` are path-independent examples. Copy the appropriate file and replace the disk-image placeholder with a local path.

```powershell
Copy-Item .mcp.json.example .mcp.json
```

`.mcp.json` and `.codex/config.toml` are intentionally ignored because they are local configuration. Do not add them, executable builds, firmware, or disk images to a public repository.

### Capability layers

`--mcp-layers` accepts a comma-separated list of `l0` through `l4`, or `all`. The default is `l0,l1`.

| Layer | Main capabilities |
|---|---|
| Control | Run, pause, reset, save, and load |
| L0 | Display capture, keyboard, mouse, joypad, and audio capture |
| L1 | Human68k console, media changes, and host directories |
| L2 | Registers, memory, breakpoints, and disassembly |
| L3 | Video, VRAM, palette, sprites, DMA, IRQ, and OPM |
| L4 | Named snapshots, rewind, and speed measurement |

Use the JSON Schema returned by `tools/list` after connecting for the exact input and output contract of each tool. L2 and higher include operations that can modify emulated state; connect only trusted MCP clients.

## Known limitations

- `arcl_type`, `arcl_command`, and `arcl_mount` cannot enter `:`, `*`, `^`, `_`, or `~`.
- `arcl_console_read` recognizes half-width characters. Full-width characters and kanji may appear as `?`.
- Instruction-level stepping through `arcl_step` is unsupported. Use `arcl_run(frames=1)` or an exec breakpoint with `arcl_run(until_break=true)`.
- Breakpoints and watchpoints stop at frame granularity.
- `x68k_opm` is frontend-maintained observation state, not part of save states. Its values may remain zero after loading a state until the guest writes OPM registers again.

## Repository layout

| Path | Contents |
|---|---|
| `windows/` | Windows frontend, MCP server, and CMake configuration |
| `px68k-libretro/` | px68k-libretro source with minimal ARCL hooks |
| `arcl_common_spec.md` | Japanese ARCL cross-machine tool contract |
| `arcl_common_spec.en.md` | English ARCL cross-machine tool contract |
| `.mcp.json.example` | MCP client configuration example |
| `.codex/config.toml.example` | Codex CLI configuration example |

## License

px68k-libretro and this frontend, which links with the core, are distributed under GPLv2. See [px68k-libretro/COPYING](px68k-libretro/COPYING) for the full license text.

BIOS/CG-ROM files and disk images belong to their respective rights holders. They are not licensed by this repository and must not be bundled or redistributed with it.
