/* px68k ARCL - Windows frontend entry point.
 *
 * See ../plan.md for phase scope. This file wires together frontend_core
 * (the libretro core wrapper), arcl_control (Control tools), arcl_l0 (L0
 * tools), mcp_server (stdio JSON-RPC), and the SDL2 window. The window is
 * created in both --mcp and interactive launches (arcl_common_spec.md 5.3:
 * GUI and AI observe/operate the same machine); in --mcp mode it runs on
 * its own thread since the main thread is blocked reading stdin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "arcl_control.h"
#include "arcl_l0.h"
#include "arcl_l1.h"
#include "arcl_l2.h"
#include "arcl_l3.h"
#include "arcl_l4.h"
#include "arcl_opm.h"
#include "arcl_watchpoint.h"
#include "frontend_core.h"
#include "libretro.h" /* RETROK_*, RETRO_DEVICE_ID_JOYPAD_* - interactive-mode input passthrough */
#include "mcp_json.h"
#include "mcp_server.h"
#include "png_write.h"

typedef struct {
    const char *content_path;
    const char *system_dir;
    const char *dump_frame_path;
    int dump_after_frames;
    int mcp_mode;
    int no_window;
    unsigned mcp_layers;
    int clock_mhz; /* 0 = unset, keep the core's own default (10Mhz) */
    int ram_mb;    /* 0 = unset, keep the core's own default (2MB) */
} px68k_args_t;

static const mcp_tool_definition_t g_control_tools[] = {
    { "arcl_run",
      "Common (arcl) Control. Advances the machine by up to `frames` and returns paused. If `text_match` is given "
      "(requires L1) it also stops as soon as that substring appears anywhere in the decoded console; if "
      "`until_break` is true (requires L2) it also stops as soon as an active arcl_breakpoint entry fires - "
      "whichever condition comes first. stop_reason is \"frames\", \"text_match\" (with matched_line), or "
      "\"breakpoint\" (with a breakpoint object: type/pc/address/value/is_write/length) accordingly. Breakpoint "
      "stop granularity is frame-level, not instruction-level (x68k_mcp.md 6.3 - the C68K backend can't stop "
      "mid-instruction). frames_used may be less than the requested frames when a stop condition fires early.",
      "{\"type\":\"object\",\"properties\":{"
      "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":36000},"
      "\"text_match\":{\"type\":\"string\"},"
      "\"until_break\":{\"type\":\"boolean\"}},\"additionalProperties\":false}", 0 },
    { "arcl_pause", "Common (arcl) Control. Stop continuous emulation without advancing the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", 0 },
    { "arcl_resume", "Common (arcl) Control. Start continuous emulation; call arcl_pause before taking deterministic actions.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", 0 },
    { "arcl_status", "Common (arcl) Control. Read machine identity, frame number, and running or paused state.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", 0 },
    { "arcl_reset", "Common (arcl) Control. Reset the X68000 core and return paused at frame zero.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}", 0 },
    { "arcl_save_state", "Common (arcl) Control. Save the current paused state in an in-memory slot.",
      "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9}},\"additionalProperties\":false}", 0 },
    { "arcl_load_state", "Common (arcl) Control. Restore an in-memory save-state slot and its frame number.",
      "{\"type\":\"object\",\"properties\":{\"slot\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":9}},\"additionalProperties\":false}", 0 }
};

static const mcp_tool_definition_t g_l0_tools[] = {
    { "arcl_screenshot",
      "Common (arcl) L0 Observation. Returns the current framebuffer, optionally cropped to x/y/w/h and scaled by "
      "an integer factor (nearest-neighbor only - no interpolation, so no colors are invented). A 256x256 screen "
      "with a 16x16 sprite cannot be judged at 1x scale; crop and scale before deciding whether a sprite is "
      "correct. grid draws magenta gridlines every N source dots (requires scale>=2). Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"x\":{\"type\":\"integer\",\"minimum\":0},\"y\":{\"type\":\"integer\",\"minimum\":0},"
      "\"w\":{\"type\":\"integer\",\"minimum\":1},\"h\":{\"type\":\"integer\",\"minimum\":1},"
      "\"scale\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":16},"
      "\"grid\":{\"type\":\"integer\",\"minimum\":0},"
      "\"path\":{\"type\":\"string\"},\"inline\":{\"type\":\"boolean\"}},\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_key",
      "Common (arcl) L0 Action. press/release hold or release one key on the emulated keyboard; tap holds it for "
      "frames (default 1) and advances the machine, returning frames_used. F12 and ScrollLock are blocked here "
      "because the core reacts to them itself (built-in menu, MIDI toggle); use the host GUI for those.",
      "{\"type\":\"object\",\"properties\":{"
      "\"key\":{\"type\":\"string\"},\"action\":{\"type\":\"string\",\"enum\":[\"press\",\"release\",\"tap\"]},"
      "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600}},"
      "\"required\":[\"key\",\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_clear_input",
      "Common (arcl) L0 Action. Releases every held key, joypad button, and mouse button, and drops any "
      "undelivered mouse motion. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_type",
      "Common (arcl) L0 Action. Types an ASCII string one character at a time (each char held for `frames`, "
      "default 2, with a 1-frame gap between characters so repeats register as separate keystrokes). "
      "Uppercase letters and shifted symbols hold LSHIFT alongside the base key, exactly like a physical "
      "keyboard. Fails atomically (nothing is typed) if any character has no mapped key - '%' has none on "
      "this keyboard model. Advances the machine and returns frames_used.",
      "{\"type\":\"object\",\"properties\":{"
      "\"text\":{\"type\":\"string\",\"maxLength\":256},"
      "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600}},"
      "\"required\":[\"text\"],\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_mouse",
      "Common (arcl) L0 Action. move accumulates a relative (dx,dy) that is delivered to the guest on the "
      "next frame actually run (see arcl_input_state's pending_dx/dy); press/release hold or release a "
      "button. Does not itself advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"move\",\"press\",\"release\"]},"
      "\"dx\":{\"type\":\"integer\"},\"dy\":{\"type\":\"integer\"},"
      "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\"]}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_joypad",
      "Common (arcl) L0 Action. press/release hold or release one button on the emulated joypad (port 0 or "
      "1, default 0); tap holds it for frames (default 1) and advances the machine, returning frames_used.",
      "{\"type\":\"object\",\"properties\":{"
      "\"port\":{\"type\":\"integer\",\"enum\":[0,1]},\"button\":{\"type\":\"string\"},"
      "\"action\":{\"type\":\"string\",\"enum\":[\"press\",\"release\",\"tap\"]},"
      "\"frames\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":3600}},"
      "\"required\":[\"button\",\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_input_state",
      "Common (arcl) L0 Observation. Reports every currently-held key and joypad button and the mouse "
      "button/pending-motion state, so a press that silently didn't take (or a release that didn't clear) "
      "can be confirmed directly instead of inferred from the screen. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_input_macro",
      "Common (arcl) L0 Action. Runs up to 16 steps in one call, each shaped exactly like the matching "
      "single tool's arguments plus an \"op\" field (key/type/mouse/joypad/clear_input/screenshot/run). "
      "Stops at the first failed step and reports how many completed (steps_completed) rather than rolling "
      "back or silently skipping - see arcl_common_spec.md 4.5. Total frames across all run/tap/type steps "
      "is capped at 10000.",
      "{\"type\":\"object\",\"properties\":{"
      "\"steps\":{\"type\":\"array\",\"maxItems\":16,\"items\":{\"type\":\"object\"}}},"
      "\"required\":[\"steps\"],\"additionalProperties\":false}",
      MCP_LAYER_L0 },
    { "arcl_audio_record",
      "Common (arcl) L0 Observation/Control. start begins capturing audio (independent of any playback path, "
      "44100Hz stereo s16, capped at 60s - further samples are dropped once full, reported via `truncated`); "
      "stop ends capture and reports frames/seconds/peak/rms/silent (peak < 256 out of 32767 counts as silent), "
      "optionally writing a WAV to `path` and/or including it inline as base64 (`inline`, default false - can "
      "be large); status reports without stopping. Typical use: start, arcl_run some frames, stop. Does not "
      "itself advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"start\",\"stop\",\"status\"]},"
      "\"path\":{\"type\":\"string\"},\"inline\":{\"type\":\"boolean\"}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L0 }
};

static const mcp_tool_definition_t g_l1_tools[] = {
    { "arcl_console_read",
      "Common (arcl) L1 Observation. Decodes the visible text screen to UTF-8 by matching each 8x16 cell against "
      "the CG ROM's half-width font (x68k_mcp.md 6.2/6.1.1). Full-width (Kanji) cells and anything else that "
      "doesn't match render as '?'; truly blank cells render as ' '. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L1 },
    { "arcl_command",
      "Common (arcl) L1 composite. Types `command` (via arcl_type), confirms the echoed prompt, sends RETURN, "
      "then advances the machine (polling in small batches, capped at 6000 frames) until the same prompt "
      "reappears on the current line. Returns the full console text once done. Decomposes into arcl_type + "
      "arcl_run + arcl_console_read (arcl_common_spec.md 7.7).",
      "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"maxLength\":180}},"
      "\"required\":[\"command\"],\"additionalProperties\":false}",
      MCP_LAYER_L1 },
    { "arcl_mount",
      "Common (arcl) L1 Action. insert swaps a new disk image path in (via the core's own multi-disk swap "
      "interface); eject removes the current one. Targets whichever drive the core defaults new images to "
      "(FDD1/\"B:\") - there is no drive-selection argument yet. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"insert\",\"eject\"]},\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L1 },
    { "arcl_host_dir",
      "Common (arcl) L1 Observation. Lists a host directory's contents (name/is_dir/size). This core has no "
      "WindrvXM-equivalent guest<->host share, so this only reads the host side at whatever permission the "
      "arcl_windows_x68k.exe process has; there is no explicit root restriction yet. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
      "\"required\":[\"path\"],\"additionalProperties\":false}",
      MCP_LAYER_L1 }
};

static const mcp_tool_definition_t g_l2_tools[] = {
    { "arcl_registers",
      "Common (arcl) L2 Observation. Returns every m68000_get_reg() register (pc/sr/sp/usp/isp/msp/vbr/sfc/dfc/"
      "cacr/caar/pref_addr/pref_data/d0-d7/a0-a7) as 0x-prefixed hex strings. On this project's C68K backend, "
      "only pc/usp/msp/sr/d0-d7/a0-a7 are actually implemented by the core; sp/isp/vbr/sfc/dfc/cacr/caar/"
      "pref_addr/pref_data always read back as 0x0 (m68000/m68000.c has no C68K case for them). a7 *is* the "
      "live stack pointer (68000 doesn't have a separate SP register - A7 is USP or SSP depending on "
      "supervisor mode), so use a7, not sp, for stack work. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_write_registers",
      "Common (arcl) L2 Action. Sets one or more named registers (same names as arcl_registers, e.g. \"pc\", "
      "\"d0\"); omitted fields are left unchanged. Writes to sp/isp/vbr/sfc/dfc/cacr/caar/pref_addr/pref_data "
      "are silently discarded by the core on this backend, same as their reads (see arcl_registers). Value may "
      "be an integer or a 0x-prefixed hex string. Returns the full register set after writing. Does not "
      "advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"pc\":{},\"sr\":{},\"sp\":{},\"usp\":{},\"isp\":{},\"msp\":{},\"vbr\":{},\"sfc\":{},\"dfc\":{},"
      "\"cacr\":{},\"caar\":{},\"pref_addr\":{},\"pref_data\":{},"
      "\"d0\":{},\"d1\":{},\"d2\":{},\"d3\":{},\"d4\":{},\"d5\":{},\"d6\":{},\"d7\":{},"
      "\"a0\":{},\"a1\":{},\"a2\":{},\"a3\":{},\"a4\":{},\"a5\":{},\"a6\":{},\"a7\":{}},"
      "\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_read_mem",
      "Common (arcl) L2 Observation. Reads `length` (default 16, max 65536) bytes starting at `address` "
      "(integer or hex string) via cpu_readmem24, same path a real CPU access takes - reading a watched address "
      "here trips any read/access watchpoint on it. Returns as a lowercase hex string. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{},\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65536}},"
      "\"required\":[\"address\"],\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_write_mem",
      "Common (arcl) L2 Action. Writes `data` (a hex string, e.g. \"deadbeef\", max 8192 bytes) starting at "
      "`address` (integer or hex string) via cpu_writemem24. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{},\"data\":{\"type\":\"string\"}},"
      "\"required\":[\"address\",\"data\"],\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_breakpoint",
      "Common (arcl) L2 Action. add/remove/clear/list breakpoints and memory watchpoints (up to 8 at once); "
      "remove takes the numeric bp_id an earlier add/list response returned (not \"id\" - this server's request "
      "parser would otherwise read the JSON-RPC envelope's own id field instead). "
      "type is exec/read/write/access; length (default 1) is the watched byte range for read/write/access. "
      "exec breakpoints are polled opportunistically on other bus accesses, not on instruction fetch itself "
      "(x68k_mcp.md 6.3), so they, like all breakpoints here, stop at frame granularity. Pair with arcl_run's "
      "until_break to actually stop on a hit. Does not itself advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"remove\",\"clear\",\"list\"]},"
      "\"type\":{\"type\":\"string\",\"enum\":[\"exec\",\"read\",\"write\",\"access\"]},"
      "\"address\":{},\"length\":{\"type\":\"integer\",\"minimum\":1},"
      "\"bp_id\":{\"type\":\"integer\",\"description\":\"id from arcl_breakpoint's add/list response; required for remove\"}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_stack",
      "Common (arcl) L2 Observation. Reads `count` (default 16, max 256) 32-bit words starting at the current "
      "SP (m68000_get_reg(M68K_SP), i.e. whichever of USP/SSP is active). Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256}},"
      "\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_disasm",
      "Common (arcl) L2 Observation. Decodes `count` (default 8, max 256) instructions starting at `address` "
      "(integer or hex string). Best-effort coverage only (no disassembler ships with this project's C68K "
      "backend): MOVE/MOVEA, MOVEQ, LEA/PEA, Bcc/BRA/BSR/DBcc, JMP/JSR/RTS/RTE/RTR/NOP/TRAP*, "
      "CLR/TST/NOT/NEG/NEGX/TAS, ADDQ/SUBQ, EXT/SWAP/LINK/UNLK/EXG, MOVEM, and register-direct-destination "
      "ADD/SUB/AND/OR/EOR/CMP/shift-rotate are decoded; anything else renders as \".dw $xxxx\" (see "
      "x68k_mcp.md 6.3). Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"address\":{},\"count\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":256}},"
      "\"required\":[\"address\"],\"additionalProperties\":false}",
      MCP_LAYER_L2 },
    { "arcl_step",
      "Common (arcl) L2 Action. Always returns an error: the C68K backend this project uses cannot stop "
      "mid-instruction (x68k_mcp.md 6.3). Use arcl_run with frames=1, or an exec arcl_breakpoint plus "
      "arcl_run's until_break, instead.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L2 }
};

static const mcp_tool_definition_t g_l3_tools[] = {
    { "arcl_video",
      "Common (arcl) L3 Observation. Decoded CRTC/video-controller state: text screen geometry (dot_x/dot_y/"
      "scroll_x/scroll_y), the 4 graphic-screen scroll pages, and CRTC vstart/vend/hstart/hend/mode/int_line. "
      "Raw CRTC_Regs (48 bytes) and VCReg0-2 are included alongside for anything not individually decoded. "
      "Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L3 },
    { "arcl_palette",
      "Common (arcl) L3 Observation. All 256 graphic-screen and 256 text-screen palette entries, decoded from "
      "the X68000's 16-bit GGGGGRRRRRBBBBBI registers to 8-bit r/g/b (standard 5->8 bit replication) plus the "
      "raw intensity bit (its half-brightness hardware behavior is not modeled). Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L3 },
    { "arcl_sprites",
      "Common (arcl) L3 Observation. All 128 hardware sprite entries (x/y/priority/palette bank/raw 16-bit "
      "ctrl word) plus the raw BG controller registers and scroll-alignment fields (bg_hadjust/bg_vline/"
      "vlinebg). ctrl's pattern-number and H/V-reverse bit layout isn't decoded with confidence (see "
      "x68k_mcp.md 6.4) so ctrl_raw is given as-is rather than guessed at. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L3 },
    { "arcl_dma",
      "Common (arcl) L3 Observation. All 4 MC68450 DMAC channels' registers (csr/cer/dcr/ocr/scr/ccr/mtc/mar/"
      "dar/btc/bar/niv/eiv/mfc/cpr/dfc/bfc/gcr - standard datasheet names). Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L3 },
    { "arcl_irq",
      "Common (arcl) L3 Observation. The MC68901 MFP's full register set (the X68000's primary interrupt "
      "controller - FDC/keyboard/timers etc. route through it: gpip/aer/ddr/iera/ierb/ipra/iprb/isra/isrb/"
      "imra/imrb/vr/timers/scr/ucr/rsr/tsr/udr) plus the separate IOC's int_stat/int_vect and the CRTC raster "
      "interrupt line. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L3 },
    { "arcl_vram",
      "Common (arcl) L3 Observation. Raw bytes from `region` (\"gvram\" or \"tvram\", each 0x80000 bytes), "
      "starting at `address` (default 0, integer or hex string) for `length` bytes (default 256, max 65536), "
      "as a lowercase hex string. This is the host-side plane storage, not CPU bus addresses - use "
      "arcl_read_mem for that. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{"
      "\"region\":{\"type\":\"string\",\"enum\":[\"gvram\",\"tvram\"]},"
      "\"address\":{},\"length\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65536}},"
      "\"required\":[\"region\"],\"additionalProperties\":false}",
      MCP_LAYER_L3 },
    { "x68k_opm",
      "X68000-specific L3 Observation. Decodes the YM2151 OPM's registers via a write shadow (the chip itself "
      "has no readback API - x68k_mcp.md 6.4): LFO (freq/pmd_amd/waveform/ct1/ct2), noise, timers A/B, CSM, "
      "and per-channel (key_on_mask/rl/fb/connect/kc_octave/kc_note/kf/pms/ams) with 4 operators each "
      "(dt1/mul/tl/ks/ar/ams_en/d1r/dt2/d2r/d1l/rr). CAVEAT: this shadow is project state, not part of the "
      "core's save state - it reads back all-zero immediately after arcl_load_state/arcl_snapshot restore "
      "until the guest writes to OPM again. Does not advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L3 }
};

static const mcp_tool_definition_t g_l4_tools[] = {
    { "arcl_snapshot",
      "Common (arcl) L4 Control. Named, in-memory snapshots (up to 32) of the full core state via "
      "retro_serialize/retro_unserialize - separate from Control's numbered arcl_save_state/arcl_load_state "
      "slots. create/restore/delete take `snapshot_name` (not \"name\" - this server's request parser would "
      "otherwise read the JSON-RPC params.name field, i.e. the tool name itself, instead); list takes none. "
      "Every response reports total_bytes (arcl_common_spec.md 6.5: state size x depth is the real constraint). "
      "create/restore briefly pause the machine if it was running, for a consistent snapshot; both leave it paused.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"restore\",\"delete\",\"list\"]},"
      "\"snapshot_name\":{\"type\":\"string\",\"maxLength\":63}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L4 },
    { "arcl_rewind",
      "Common (arcl) L4 Control. Auto-captured full-state rewind history, separate from arcl_snapshot. enable "
      "(re)starts capturing a state snapshot every `interval` frames (default 60, min 10) into a fixed ring of "
      "8 slots - every frame-advancing tool (arcl_run, arcl_key tap, arcl_type, ...) can trigger a capture, not "
      "just arcl_run. disable stops and frees the ring. status reports enabled/interval/entries/total_bytes "
      "(arcl_common_spec.md 6.5: report memory consumption). rewind restores `steps` captures back (default 1) "
      "and discards newer history beyond that point, like an undo stack; errors if fewer than `steps` captures "
      "exist yet. rewind pauses the machine first.",
      "{\"type\":\"object\",\"properties\":{"
      "\"action\":{\"type\":\"string\",\"enum\":[\"enable\",\"disable\",\"status\",\"rewind\"]},"
      "\"interval\":{\"type\":\"integer\",\"minimum\":10,\"maximum\":36000},"
      "\"steps\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},"
      "\"required\":[\"action\"],\"additionalProperties\":false}",
      MCP_LAYER_L4 },
    { "arcl_speed",
      "Common (arcl) L4 Control. Real-time ratio of the current (or, if paused, the just-ended) arcl_resume "
      "session: fps is frames actually advanced per wall-clock second since that resume started, nominal_fps "
      "is the core's configured target, speed_percent is fps/nominal_fps*100. running is false (and the other "
      "fields 0) if nothing has been resumed yet in this session. Does not itself advance the machine.",
      "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
      MCP_LAYER_L4 }
};

typedef struct {
    arcl_control_t *control;
    arcl_l0_t *l0;
    arcl_l1_t *l1;
    arcl_l2_t *l2;
    arcl_l3_t *l3;
    arcl_l4_t *l4;
    /* Same value as mcp_server_config_t.enabled_layers (--mcp-layers) -
     * duplicated here because dispatch_tool_call() only receives `userdata`
     * (this struct), not the server config. Needed so tool *execution*, not
     * just tools/list, actually respects the flag - see the enforcement
     * check below. */
    unsigned enabled_layers;
} tool_context_t;

static const mcp_tool_definition_t *find_tool(const char *name, const mcp_tool_definition_t *tools, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (strcmp(tools[i].name, name) == 0)
            return &tools[i];
    return NULL;
}

/* mcp_server.c's send_tools_list() already hides tools whose layer isn't in
 * enabled_layers, but that's advisory only - it filters what's *advertised*
 * via tools/list, not what tools/call will actually run. Before this check
 * existed, a client that already knew a tool's name (e.g. from
 * x68k_mcp.md, or a previous session with `all` enabled) could call
 * arcl_write_mem/arcl_breakpoint/etc. even when launched with the
 * conservative default `--mcp-layers l0,l1`, making the flag purely
 * cosmetic for anything but discovery. Control tools have layer_mask == 0
 * (always allowed, per the "Control tools use layer_mask == 0" convention
 * in send_tools_list()), so they pass through unconditionally here too. */
static int layer_enabled(const tool_context_t *ctx, const mcp_tool_definition_t *tool,
                          char *error_message, size_t error_size)
{
    if (tool->layer_mask && !(tool->layer_mask & ctx->enabled_layers))
    {
        mcp_json_set_error(error_message, error_size,
            "this tool's layer was not enabled via --mcp-layers for this session (see tools/list for what's available)");
        return 0;
    }
    return 1;
}

static int dispatch_tool_call(const char *name, const char *request_json, void *userdata,
                               char *result_json, size_t result_size,
                               char *error_message, size_t error_size)
{
    tool_context_t *ctx = (tool_context_t *)userdata;
    const mcp_tool_definition_t *tool;

    if (find_tool(name, g_control_tools, sizeof(g_control_tools) / sizeof(g_control_tools[0])))
        return arcl_control_call(name, request_json, ctx->control, result_json, result_size, error_message, error_size);
    if ((tool = find_tool(name, g_l0_tools, sizeof(g_l0_tools) / sizeof(g_l0_tools[0]))) != NULL)
        return layer_enabled(ctx, tool, error_message, error_size) &&
               arcl_l0_call(name, request_json, ctx->l0, result_json, result_size, error_message, error_size);
    if ((tool = find_tool(name, g_l1_tools, sizeof(g_l1_tools) / sizeof(g_l1_tools[0]))) != NULL)
        return layer_enabled(ctx, tool, error_message, error_size) &&
               arcl_l1_call(name, request_json, ctx->l1, result_json, result_size, error_message, error_size);
    if ((tool = find_tool(name, g_l2_tools, sizeof(g_l2_tools) / sizeof(g_l2_tools[0]))) != NULL)
        return layer_enabled(ctx, tool, error_message, error_size) &&
               arcl_l2_call(name, request_json, ctx->l2, result_json, result_size, error_message, error_size);
    if ((tool = find_tool(name, g_l3_tools, sizeof(g_l3_tools) / sizeof(g_l3_tools[0]))) != NULL)
        return layer_enabled(ctx, tool, error_message, error_size) &&
               arcl_l3_call(name, request_json, ctx->l3, result_json, result_size, error_message, error_size);
    if ((tool = find_tool(name, g_l4_tools, sizeof(g_l4_tools) / sizeof(g_l4_tools[0]))) != NULL)
        return layer_enabled(ctx, tool, error_message, error_size) &&
               arcl_l4_call(name, request_json, ctx->l4, result_json, result_size, error_message, error_size);
    if (error_size)
    {
        strncpy(error_message, "Unknown tool", error_size - 1);
        error_message[error_size - 1] = '\0';
    }
    return 0;
}

static void to_absolute(const char *in, char *out, size_t out_size)
{
    if (!GetFullPathNameA(in, (DWORD)out_size, out, NULL))
    {
        strncpy(out, in, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static int parse_args(int argc, char **argv, px68k_args_t *args)
{
    memset(args, 0, sizeof(*args));
    args->system_dir = "px68k/system";
    args->dump_after_frames = 1200;
    args->mcp_layers = MCP_LAYER_L0 | MCP_LAYER_L1;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--system-dir") == 0 && i + 1 < argc)
            args->system_dir = argv[++i];
        else if (strcmp(argv[i], "--dump-frame") == 0 && i + 1 < argc)
            args->dump_frame_path = argv[++i];
        else if (strcmp(argv[i], "--dump-after-frames") == 0 && i + 1 < argc)
            args->dump_after_frames = atoi(argv[++i]);
        else if (strcmp(argv[i], "--mcp") == 0)
            args->mcp_mode = 1;
        else if (strcmp(argv[i], "--no-window") == 0)
            args->no_window = 1;
        else if (strcmp(argv[i], "--mcp-layers") == 0 && i + 1 < argc)
        {
            if (!mcp_server_parse_layers(argv[++i], &args->mcp_layers))
            {
                fprintf(stderr, "invalid --mcp-layers value (use l0,l1,l2,l3,l4 or all)\n");
                return 0;
            }
        }
        else if (strcmp(argv[i], "--clock") == 0 && i + 1 < argc)
            args->clock_mhz = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc)
            args->ram_mb = atoi(argv[++i]);
        else if (argv[i][0] != '-')
            args->content_path = argv[i];
        else
        {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 0;
        }
    }
    return 1;
}

/* Interactive-mode keyboard passthrough (x68k_mcp.md 7.3: GUI key input
 * and MCP's arcl_key/arcl_type merge into the same held-key state -
 * previously true in name only, since this loop only ever handled its own
 * 4 reserved keys and forwarded nothing else to the guest).
 *
 * SDL2 kept SDL1.2-compatible values for its low keycodes: SDLK_a == 'a',
 * SDLK_RETURN == 13, SDLK_ESCAPE == 27, etc. libretro's RETROK_* enum
 * (libretro.h) was modeled on that exact same SDL1.2 keysym table, so for
 * the printable/control-character range (0-127) SDLK_X and RETROK_X are
 * numerically identical - no table needed, a raw cast is correct. Only
 * the extended keys (arrows, F-keys, keypad, modifiers, ...) need mapping,
 * since SDL2 encodes those as SDL_SCANCODE_MASK|scancode, a completely
 * different numbering from libretro's 256+ range. */
static unsigned sdl_keycode_to_retrok(SDL_Keycode sym)
{
    if (sym >= 0 && sym < 128)
        return (unsigned)sym;
    switch (sym)
    {
    case SDLK_KP_0: return RETROK_KP0;
    case SDLK_KP_1: return RETROK_KP1;
    case SDLK_KP_2: return RETROK_KP2;
    case SDLK_KP_3: return RETROK_KP3;
    case SDLK_KP_4: return RETROK_KP4;
    case SDLK_KP_5: return RETROK_KP5;
    case SDLK_KP_6: return RETROK_KP6;
    case SDLK_KP_7: return RETROK_KP7;
    case SDLK_KP_8: return RETROK_KP8;
    case SDLK_KP_9: return RETROK_KP9;
    case SDLK_KP_PERIOD: return RETROK_KP_PERIOD;
    case SDLK_KP_DIVIDE: return RETROK_KP_DIVIDE;
    case SDLK_KP_MULTIPLY: return RETROK_KP_MULTIPLY;
    case SDLK_KP_MINUS: return RETROK_KP_MINUS;
    case SDLK_KP_PLUS: return RETROK_KP_PLUS;
    case SDLK_KP_ENTER: return RETROK_KP_ENTER;
    case SDLK_KP_EQUALS: return RETROK_KP_EQUALS;
    case SDLK_UP: return RETROK_UP;
    case SDLK_DOWN: return RETROK_DOWN;
    case SDLK_RIGHT: return RETROK_RIGHT;
    case SDLK_LEFT: return RETROK_LEFT;
    case SDLK_INSERT: return RETROK_INSERT;
    case SDLK_HOME: return RETROK_HOME;
    case SDLK_END: return RETROK_END;
    case SDLK_PAGEUP: return RETROK_PAGEUP;
    case SDLK_PAGEDOWN: return RETROK_PAGEDOWN;
    case SDLK_F1: return RETROK_F1;
    case SDLK_F2: return RETROK_F2;
    case SDLK_F3: return RETROK_F3;
    case SDLK_F4: return RETROK_F4;
    case SDLK_F5: return RETROK_F5;
    case SDLK_F6: return RETROK_F6;
    case SDLK_F7: return RETROK_F7;
    case SDLK_F8: return RETROK_F8;
    case SDLK_F9: return RETROK_F9;
    case SDLK_F10: return RETROK_F10;
    case SDLK_F11: return RETROK_F11;
    case SDLK_F12: return RETROK_F12;
    case SDLK_F13: return RETROK_F13;
    case SDLK_F14: return RETROK_F14;
    case SDLK_F15: return RETROK_F15;
    case SDLK_NUMLOCKCLEAR: return RETROK_NUMLOCK;
    case SDLK_CAPSLOCK: return RETROK_CAPSLOCK;
    case SDLK_SCROLLLOCK: return RETROK_SCROLLOCK;
    case SDLK_RSHIFT: return RETROK_RSHIFT;
    case SDLK_LSHIFT: return RETROK_LSHIFT;
    case SDLK_RCTRL: return RETROK_RCTRL;
    case SDLK_LCTRL: return RETROK_LCTRL;
    case SDLK_RALT: return RETROK_RALT;
    case SDLK_LALT: return RETROK_LALT;
    case SDLK_RGUI: return RETROK_RSUPER;
    case SDLK_LGUI: return RETROK_LSUPER;
    case SDLK_MODE: return RETROK_MODE;
    case SDLK_HELP: return RETROK_HELP;
    case SDLK_PRINTSCREEN: return RETROK_PRINT;
    case SDLK_SYSREQ: return RETROK_SYSREQ;
    case SDLK_MENU: return RETROK_MENU;
    case SDLK_POWER: return RETROK_POWER;
    case SDLK_UNDO: return RETROK_UNDO;
    default: return RETROK_UNKNOWN;
    }
}

/* SDL_GameController -> RETRO_DEVICE_ID_JOYPAD_* using the standard
 * RetroPad<->XInput face-button convention (also used by every other
 * libretro frontend): SDL's bottom/right/left/top face buttons swap
 * relative to their libretro B/A/Y/X counterparts. Analog sticks aren't
 * mapped (buttons/d-pad only) - kept out of scope for this pass, since
 * turning continuous axis motion into digital UP/DOWN/LEFT/RIGHT needs
 * its own edge-tracking state to coexist cleanly with real d-pad presses. */
static int sdl_controller_button_to_joypad(SDL_GameControllerButton b, unsigned *out_id)
{
    switch (b)
    {
    case SDL_CONTROLLER_BUTTON_A: *out_id = RETRO_DEVICE_ID_JOYPAD_B; return 1;
    case SDL_CONTROLLER_BUTTON_B: *out_id = RETRO_DEVICE_ID_JOYPAD_A; return 1;
    case SDL_CONTROLLER_BUTTON_X: *out_id = RETRO_DEVICE_ID_JOYPAD_Y; return 1;
    case SDL_CONTROLLER_BUTTON_Y: *out_id = RETRO_DEVICE_ID_JOYPAD_X; return 1;
    case SDL_CONTROLLER_BUTTON_BACK: *out_id = RETRO_DEVICE_ID_JOYPAD_SELECT; return 1;
    case SDL_CONTROLLER_BUTTON_START: *out_id = RETRO_DEVICE_ID_JOYPAD_START; return 1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: *out_id = RETRO_DEVICE_ID_JOYPAD_L3; return 1;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: *out_id = RETRO_DEVICE_ID_JOYPAD_R3; return 1;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: *out_id = RETRO_DEVICE_ID_JOYPAD_L; return 1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: *out_id = RETRO_DEVICE_ID_JOYPAD_R; return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: *out_id = RETRO_DEVICE_ID_JOYPAD_UP; return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: *out_id = RETRO_DEVICE_ID_JOYPAD_DOWN; return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: *out_id = RETRO_DEVICE_ID_JOYPAD_LEFT; return 1;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: *out_id = RETRO_DEVICE_ID_JOYPAD_RIGHT; return 1;
    default: return 0;
    }
}

static void save_timestamped_screenshot(void)
{
    px68k_frame_t frame = px68k_frontend_get_frame();
    if (frame.valid)
    {
        SYSTEMTIME st;
        char path[64];
        GetLocalTime(&st);
        snprintf(path, sizeof(path), "px68k_%04d%02d%02d_%02d%02d%02d.png",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        px68k_write_png_rgb565(path, frame.pixels, frame.width, frame.height, frame.stride);
    }
}

/* --- MCP-mode GUI thread -----------------------------------------------
 * stdin/stdout are owned by mcp_server_run() on the main thread, so the
 * SDL window, its event pump, and its render loop all live here instead
 * (arcl_common_spec.md 5.3 / x68k_mcp.md 7.1: GUI stays available while
 * MCP drives the machine). F5 mirrors arcl_pause/arcl_resume through the
 * same arcl_control_t the MCP tools use, so pressing it in the window and
 * calling the tool converge on one shared state (x68k_mcp.md 7.3). The
 * window's close button is ignored here on purpose: only stdin closing
 * ends an MCP session, so a stray click can't kill it out from under an
 * AI client. */
typedef struct {
    arcl_control_t *control;
    volatile LONG stop;
    HANDLE thread;
} gui_thread_ctx_t;

static DWORD WINAPI gui_thread_proc(LPVOID opaque)
{
    gui_thread_ctx_t *ctx = (gui_thread_ctx_t *)opaque;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int show_detail_title = 0;
    int title_tick = 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window = SDL_CreateWindow("px68k ARCL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               800, 600, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (renderer)
        SDL_RenderSetLogicalSize(renderer, 800, 600);
    texture = renderer ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 800, 600) : NULL;
    if (!window || !renderer || !texture)
    {
        fprintf(stderr, "SDL window setup failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    while (!ctx->stop)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_KEYDOWN)
            {
                if (ev.key.keysym.sym == SDLK_F5)
                {
                    unsigned long long frame;
                    int running;
                    char result[256];
                    char error[256];
                    arcl_control_get_status(ctx->control, &frame, &running);
                    arcl_control_call(running ? "arcl_pause" : "arcl_resume", "{}", ctx->control,
                                       result, sizeof(result), error, sizeof(error));
                }
                else if (ev.key.keysym.sym == SDLK_F1)
                    show_detail_title = !show_detail_title;
                else if (ev.key.keysym.sym == SDLK_F2)
                    save_timestamped_screenshot();
            }
            /* SDL_QUIT (window close button) intentionally ignored - see comment above. */
        }

        {
            px68k_frame_t frame = px68k_frontend_get_frame();
            if (frame.valid)
            {
                SDL_Rect dst = { 0, 0, (int)frame.width, (int)frame.height };
                SDL_UpdateTexture(texture, NULL, frame.pixels, (int)(frame.stride * sizeof(uint16_t)));
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, &dst, NULL);
                SDL_RenderPresent(renderer);
            }
        }

        if (++title_tick >= 10) /* update ~once per 100ms, not every 10ms tick */
        {
            unsigned long long frame_no;
            int running;
            char title[128];
            title_tick = 0;
            arcl_control_get_status(ctx->control, &frame_no, &running);
            if (show_detail_title)
                snprintf(title, sizeof(title), "px68k ARCL - frame %llu [%s]",
                         frame_no, running ? "running" : "paused");
            else
                snprintf(title, sizeof(title), "px68k ARCL");
            SDL_SetWindowTitle(window, title);
        }

        SDL_Delay(10);
    }

cleanup:
    if (texture) SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

static int run_mcp_mode(const px68k_args_t *args)
{
    arcl_control_t *control = NULL;
    arcl_l0_t *l0 = NULL;
    arcl_l1_t *l1 = NULL;
    arcl_l2_t *l2 = NULL;
    arcl_l3_t *l3 = NULL;
    arcl_l4_t *l4 = NULL;
    tool_context_t ctx;
    gui_thread_ctx_t gui;
    mcp_server_config_t server;
    mcp_tool_definition_t all_tools[sizeof(g_control_tools) / sizeof(g_control_tools[0]) +
                                     sizeof(g_l0_tools) / sizeof(g_l0_tools[0]) +
                                     sizeof(g_l1_tools) / sizeof(g_l1_tools[0]) +
                                     sizeof(g_l2_tools) / sizeof(g_l2_tools[0]) +
                                     sizeof(g_l3_tools) / sizeof(g_l3_tools[0]) +
                                     sizeof(g_l4_tools) / sizeof(g_l4_tools[0])];
    size_t n = 0;
    int result;

    for (size_t i = 0; i < sizeof(g_control_tools) / sizeof(g_control_tools[0]); i++)
        all_tools[n++] = g_control_tools[i];
    for (size_t i = 0; i < sizeof(g_l0_tools) / sizeof(g_l0_tools[0]); i++)
        all_tools[n++] = g_l0_tools[i];
    for (size_t i = 0; i < sizeof(g_l1_tools) / sizeof(g_l1_tools[0]); i++)
        all_tools[n++] = g_l1_tools[i];
    for (size_t i = 0; i < sizeof(g_l2_tools) / sizeof(g_l2_tools[0]); i++)
        all_tools[n++] = g_l2_tools[i];
    for (size_t i = 0; i < sizeof(g_l3_tools) / sizeof(g_l3_tools[0]); i++)
        all_tools[n++] = g_l3_tools[i];
    for (size_t i = 0; i < sizeof(g_l4_tools) / sizeof(g_l4_tools[0]); i++)
        all_tools[n++] = g_l4_tools[i];

    if (!arcl_control_init(&control))
    {
        fprintf(stderr, "failed to initialize ARCL control state\n");
        return 1;
    }
    if (!arcl_l0_init(&l0, control))
    {
        fprintf(stderr, "failed to initialize ARCL L0 state\n");
        arcl_control_shutdown(control);
        return 1;
    }
    if (!arcl_l1_init(&l1, control, l0))
    {
        fprintf(stderr, "failed to initialize ARCL L1 state\n");
        arcl_l0_shutdown(l0);
        arcl_control_shutdown(control);
        return 1;
    }
    if (!arcl_l2_init(&l2, control))
    {
        fprintf(stderr, "failed to initialize ARCL L2 state\n");
        arcl_l1_shutdown(l1);
        arcl_l0_shutdown(l0);
        arcl_control_shutdown(control);
        return 1;
    }
    if (!arcl_l3_init(&l3, control))
    {
        fprintf(stderr, "failed to initialize ARCL L3 state\n");
        arcl_l2_shutdown(l2);
        arcl_l1_shutdown(l1);
        arcl_l0_shutdown(l0);
        arcl_control_shutdown(control);
        return 1;
    }
    if (!arcl_l4_init(&l4, control))
    {
        fprintf(stderr, "failed to initialize ARCL L4 state\n");
        arcl_l3_shutdown(l3);
        arcl_l2_shutdown(l2);
        arcl_l1_shutdown(l1);
        arcl_l0_shutdown(l0);
        arcl_control_shutdown(control);
        return 1;
    }
    ctx.control = control;
    ctx.l0 = l0;
    ctx.l1 = l1;
    ctx.l2 = l2;
    ctx.l3 = l3;
    ctx.l4 = l4;
    ctx.enabled_layers = args->mcp_layers;

    memset(&gui, 0, sizeof(gui));
    gui.control = control;
    if (!args->no_window)
    {
        gui.thread = CreateThread(NULL, 0, gui_thread_proc, &gui, 0, NULL);
        if (!gui.thread)
            fprintf(stderr, "warning: failed to start GUI thread; continuing MCP-only (no window)\n");
    }

    server.enabled_layers = args->mcp_layers;
    server.tools = all_tools;
    server.tool_count = n;
    server.call_tool = dispatch_tool_call;
    server.userdata = &ctx;
    result = mcp_server_run(stdin, stdout, &server);

    if (gui.thread)
    {
        InterlockedExchange(&gui.stop, 1);
        WaitForSingleObject(gui.thread, INFINITE);
        CloseHandle(gui.thread);
    }
    arcl_l4_shutdown(l4);
    arcl_l3_shutdown(l3);
    arcl_l2_shutdown(l2);
    arcl_l1_shutdown(l1);
    arcl_l0_shutdown(l0);
    arcl_control_shutdown(control);
    return result;
}

int main(int argc, char **argv)
{
    px68k_args_t args;
    char system_dir_abs[1024];
    char content_path_abs[1024];

    if (!parse_args(argc, argv, &args))
    {
        fprintf(stderr,
            "usage: %s [--mcp] [--no-window] [--mcp-layers l0,l1,...|all] [--system-dir DIR] [--clock MHZ] [--ram MB] [--dump-frame PATH.png] [--dump-after-frames N] CONTENT\n"
            "  --no-window   Run without opening an SDL2 GUI window (headless mode)\n"
            "  --clock MHZ   CPU clock speed: one of 10, 16, 25, 33, 66, 100 (default: 10)\n"
            "  --ram MB      Main RAM size in MB, 1-12 (default: 2)\n",
            argv[0]);
        return 1;
    }

    to_absolute(args.system_dir, system_dir_abs, sizeof(system_dir_abs));
    px68k_frontend_set_system_dir(system_dir_abs);
    px68k_frontend_set_save_dir(system_dir_abs);
    if (args.mcp_mode)
        px68k_frontend_set_log_path("px68k-arcl.log");

    if (args.clock_mhz && !px68k_frontend_set_clock_mhz(args.clock_mhz))
    {
        fprintf(stderr, "invalid --clock value (use 10, 16, 25, 33, 66, or 100)\n");
        return 1;
    }
    if (args.ram_mb && !px68k_frontend_set_ram_size_mb(args.ram_mb))
    {
        fprintf(stderr, "invalid --ram value (use 1-12)\n");
        return 1;
    }

    if (!px68k_frontend_init())
    {
        fprintf(stderr, "px68k_frontend_init failed\n");
        return 1;
    }

    /* ARCL_WATCHPOINT/ARCL_OPM_SHADOW hooks (mem_wrap.c/fmg_wrap.cpp) are
     * compiled into the core unconditionally and fire from the very first
     * retro_run(), in *both* --mcp and interactive launches - unlike the
     * arcl_l2/l3 MCP tool state that reads them, which only exists in
     * --mcp mode. Initializing this here (not inside arcl_l2_init(), which
     * interactive mode never calls) is what makes that safe: without it,
     * arcl_opm_shadow_on_write() dereferences a NULL lock the moment
     * Human68k's boot sequence writes to the OPM, crashing interactive
     * mode outright (x68k_mcp.md 0.1.1 - found via a real user report). */
    arcl_watchpoint_init();
    arcl_opm_shadow_init();

    if (args.content_path)
    {
        to_absolute(args.content_path, content_path_abs, sizeof(content_path_abs));
        if (!px68k_frontend_load(content_path_abs))
        {
            fprintf(stderr, "failed to load content: %s\n", content_path_abs);
            return 1;
        }
    }
    else if (!px68k_frontend_load(NULL))
    {
        fprintf(stderr, "px68k_frontend_load(NULL) failed\n");
        return 1;
    }

    if (args.mcp_mode)
    {
        /* stdout is reserved exclusively for newline-delimited MCP messages.
         * The core remains paused until a later Control tool invokes run_frame. */
        int result = run_mcp_mode(&args);
        arcl_opm_shadow_shutdown();
        arcl_watchpoint_shutdown();
        px68k_frontend_shutdown();
        return result;
    }

    /* Headless non-MCP mode (e.g. batch --dump-frame without opening a window) */
    if (args.no_window)
    {
        long frame_count = 0;
        int target_frames = args.dump_frame_path ? args.dump_after_frames : 0;
        if (target_frames <= 0)
        {
            fprintf(stderr, "error: --no-window in non-MCP mode requires --dump-frame\n");
            arcl_opm_shadow_shutdown();
            arcl_watchpoint_shutdown();
            px68k_frontend_shutdown();
            return 1;
        }

        while (frame_count < target_frames)
        {
            px68k_frontend_run_frame();
            frame_count++;
        }

        if (args.dump_frame_path)
        {
            px68k_frame_t frame = px68k_frontend_get_frame();
            if (frame.valid)
            {
                bool ok = px68k_write_png_rgb565(args.dump_frame_path, frame.pixels,
                                                  frame.width, frame.height, frame.stride);
                fprintf(stderr, "dump-frame: %s (%ux%u) -> %s\n",
                        ok ? "wrote" : "FAILED to write", frame.width, frame.height,
                        args.dump_frame_path);
            }
            else
            {
                fprintf(stderr, "dump-frame: no valid frame yet after %ld run_frame() calls\n", frame_count);
            }
        }

        arcl_opm_shadow_shutdown();
        arcl_watchpoint_shutdown();
        px68k_frontend_shutdown();
        return 0;
    }

    /* Interactive (no --mcp, no --no-window): single-threaded loop, human-only. */
    {
        SDL_Window *window;
        SDL_Renderer *renderer;
        SDL_Texture *texture;
        SDL_GameController *controller = NULL;
        int running = 1;
        int paused = 0;
        int show_detail_title = 0;
        int title_tick = 0;
        long frame_count = 0;
        int dumped = 0;
        Uint32 frame_period_ms = 16; /* recomputed every iteration below from the core's live fps */
        /* Below this many queued bytes (~40ms), the SDL audio device is
         * close enough to running dry that the next SDL_QueueAudio() push
         * might arrive too late to avoid an audible gap (x68k_mcp.md 7.x -
         * a real user-reported crackling/popping bug, root-caused via
         * arcl_audio_record capture analysis showing the *generated*
         * waveform is clean, so the gap was being introduced downstream
         * by pacing, not by audio synthesis). */
#define PX68K_AUDIO_LOW_WATER_BYTES ((PX68K_AUDIO_SAMPLE_RATE * 2 * (int)sizeof(int16_t) * 40) / 1000)

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        {
            fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        window = SDL_CreateWindow("px68k ARCL", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                   800, 600, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            return 1;
        }
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer)
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer)
        {
            fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return 1;
        }
        SDL_RenderSetLogicalSize(renderer, 800, 600);

        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 800, 600);
        if (!texture)
        {
            fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
            return 1;
        }

        /* Interactive-mode-only additions (main.c owns these, not
         * frontend_core): real audio output and a physical joypad/
         * gamepad, neither of which an MCP session needs. Both fail soft
         * - no speakers or no controller just means the emulator runs
         * silently / keyboard-only, not a startup error. */
        if (!px68k_frontend_enable_audio_output())
            fprintf(stderr, "warning: no audio output device available; running silently\n");

        {
            int i;
            for (i = 0; i < SDL_NumJoysticks(); i++)
            {
                if (SDL_IsGameController(i))
                {
                    controller = SDL_GameControllerOpen(i);
                    break;
                }
            }
        }

        while (running)
        {
            Uint32 loop_start_ms = SDL_GetTicks();
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
            {
                if (ev.type == SDL_QUIT)
                    running = 0;
                else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP)
                {
                    int pressed = (ev.type == SDL_KEYDOWN);
                    /* Esc/F5/F1/F2 stay host-reserved (x68k_mcp.md 7.2) -
                     * on keydown only, and never also forwarded to the
                     * guest. Every other key (keydown *and* keyup, so
                     * held state is accurate) merges into the same
                     * held-key array arcl_key/arcl_type use, per 7.3. */
                    if (pressed && ev.key.keysym.sym == SDLK_ESCAPE)
                        running = 0;
                    else if (pressed && ev.key.keysym.sym == SDLK_F5)
                        paused = !paused;
                    else if (pressed && ev.key.keysym.sym == SDLK_F1)
                        show_detail_title = !show_detail_title;
                    else if (pressed && ev.key.keysym.sym == SDLK_F2)
                        save_timestamped_screenshot();
                    else if (ev.key.keysym.sym != SDLK_ESCAPE && ev.key.keysym.sym != SDLK_F5 &&
                             ev.key.keysym.sym != SDLK_F1 && ev.key.keysym.sym != SDLK_F2 && !ev.key.repeat)
                    {
                        unsigned retrok = sdl_keycode_to_retrok(ev.key.keysym.sym);
                        if (retrok != RETROK_UNKNOWN)
                            px68k_frontend_set_key(retrok, pressed);
                    }
                }
                else if (ev.type == SDL_CONTROLLERDEVICEADDED && !controller)
                    controller = SDL_GameControllerOpen(ev.cdevice.which);
                else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                         ev.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)))
                {
                    SDL_GameControllerClose(controller);
                    controller = NULL;
                }
                else if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP)
                {
                    unsigned id;
                    if (sdl_controller_button_to_joypad((SDL_GameControllerButton)ev.cbutton.button, &id))
                        px68k_frontend_set_joypad(0, id, ev.type == SDL_CONTROLLERBUTTONDOWN);
                }
            }

            if (!paused)
            {
                px68k_frontend_run_frame();
                frame_count++;
            }

            px68k_frame_t frame = px68k_frontend_get_frame();
            if (frame.valid)
            {
                SDL_Rect dst = { 0, 0, (int)frame.width, (int)frame.height };
                SDL_UpdateTexture(texture, NULL, frame.pixels, (int)(frame.stride * sizeof(uint16_t)));
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, &dst, NULL);
                SDL_RenderPresent(renderer);
            }

            if (++title_tick >= 10) /* update ~once per 160ms at 16ms/tick, not every tick */
            {
                char title[128];
                title_tick = 0;
                if (show_detail_title)
                    snprintf(title, sizeof(title), "px68k ARCL - frame %ld [%s]",
                             frame_count, paused ? "paused" : "running");
                else
                    snprintf(title, sizeof(title), "px68k ARCL");
                SDL_SetWindowTitle(window, title);
            }

            if (args.dump_frame_path && !dumped && frame_count >= args.dump_after_frames)
            {
                if (frame.valid)
                {
                    bool ok = px68k_write_png_rgb565(args.dump_frame_path, frame.pixels,
                                                      frame.width, frame.height, frame.stride);
                    fprintf(stderr, "dump-frame: %s (%ux%u) -> %s\n",
                            ok ? "wrote" : "FAILED to write", frame.width, frame.height,
                            args.dump_frame_path);
                }
                else
                {
                    fprintf(stderr, "dump-frame: no valid frame yet after %ld run_frame() calls\n", frame_count);
                }
                dumped  = 1;
                running = 0;
            }

            if (!args.dump_frame_path)
            {
                /* Re-read every iteration, not just once before the loop:
                 * the core reports a fresh fps via
                 * RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO (libretro.c's
                 * CHANGEAV_TIMING branch) whenever the guest switches CRTC
                 * video modes - title screen vs. gameplay vs. ranking
                 * screen commonly differ (e.g. measured ~55.5Hz at the
                 * Human68k prompt vs. ~61.5Hz once a game like Cho Ren Sha
                 * 68K is actually running). A one-time snapshot taken
                 * before the loop starts goes stale the moment the first
                 * such switch happens, silently pacing the loop - and thus
                 * how fast audio gets queued - against the wrong rate for
                 * the rest of the session. That mismatch has nowhere to
                 * go: every frame it persists, the live audio queue drifts
                 * further from real time, which is heard as sound effects
                 * (and everything else) landing later and later relative
                 * to what's on screen. This fixes the *rate* mismatch;
                 * PX68K_AUDIO_LOW_WATER_BYTES below still exists separately
                 * to catch plain buffer underrun (crackling). */
                double fps = px68k_frontend_get_fps();
                if (fps > 1.0 && fps < 240.0)
                    frame_period_ms = (Uint32)(1000.0 / fps + 0.5);

                /* If the audio device is running low, skip this
                 * iteration's delay outright so the loop catches up (runs
                 * back-to-back, bounded only by real processing cost) and
                 * refills the queue before it actually goes dry - rather
                 * than sleeping a fixed amount regardless of buffer state
                 * and finding out only after the pop happened. */
                size_t queued = px68k_frontend_get_audio_queued_bytes();
                int audio_low = queued > 0 && queued < PX68K_AUDIO_LOW_WATER_BYTES;
                if (!audio_low)
                {
                    Uint32 elapsed = SDL_GetTicks() - loop_start_ms;
                    if (elapsed < frame_period_ms)
                        SDL_Delay(frame_period_ms - elapsed);
                }
            }
        }

        if (controller)
            SDL_GameControllerClose(controller);
        arcl_opm_shadow_shutdown();
        arcl_watchpoint_shutdown();
        px68k_frontend_shutdown();
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

    return 0;
}
