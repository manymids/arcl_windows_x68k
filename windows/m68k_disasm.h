#ifndef PX68K_M68K_DISASM_H
#define PX68K_M68K_DISASM_H

#include <stdint.h>
#include <stddef.h>

/* Best-effort 68000 disassembler for arcl_disasm (x68k_mcp.md 6.3/8 Phase 3).
 * No disassembler ships with the C68K backend this project uses (only the
 * Musashi backend has one, and that backend is out of scope - x68k_mcp.md
 * 0.1/1.1), so this is a from-scratch, intentionally-reduced-coverage
 * decoder: MOVE/MOVEA, MOVEQ, LEA/PEA, branches (Bcc/BRA/BSR/DBcc),
 * JMP/JSR/RTS/RTE/RTR/NOP/TRAP*, CLR/TST/NOT/NEG/NEGX/TAS, ADDQ/SUBQ,
 * EXT/SWAP/LINK/UNLK/EXG, MOVEM, and the register-direct-destination forms
 * of ADD/SUB/AND/OR/EOR/CMP/shift-rotate. Anything else decodes as a raw
 * ".dw $xxxx" (one word, mnemonic unknown) rather than being guessed at -
 * see x68k_mcp.md 6.3 for the full list of what this does and doesn't
 * cover and why.
 *
 * Decodes exactly one instruction at `addr` (reading guest memory through
 * cpu_readmem24_word/dword, so it sees whatever is actually mapped there -
 * ROM, RAM, or otherwise). Writes "mnemonic operands" into out. Returns
 * the instruction length in bytes (always >= 2, even for the ".dw"
 * fallback, so callers can always advance and make progress). */
uint32_t m68k_disasm_one(uint32_t addr, char *out, size_t out_size);

#endif /* PX68K_M68K_DISASM_H */
