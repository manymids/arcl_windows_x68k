#include "m68k_disasm.h"

#include <stdio.h>
#include <string.h>

#include "x68kmemory.h" /* cpu_readmem24_word/_dword */

static const char *CC_NAMES[16] = {
    "t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
    "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"
};

static uint16_t fetch16(uint32_t *pc)
{
    uint16_t v = (uint16_t)cpu_readmem24_word(*pc);
    *pc += 2;
    return v;
}

static uint32_t fetch32(uint32_t *pc)
{
    uint32_t v = cpu_readmem24_dword(*pc);
    *pc += 4;
    return v;
}

static char size_letter(int size)
{
    return size == 0 ? 'b' : size == 1 ? 'w' : 'l';
}

/* 68000 only has the "brief" extension word format (no full/68020 extension
 * words) - one index register, 8-bit displacement. */
static void decode_brief_ext(uint16_t ext, char *idx_out, size_t idx_cap, int *disp8)
{
    int is_addr = (ext >> 15) & 1;
    int reg = (ext >> 12) & 7;
    int is_long = (ext >> 11) & 1;
    *disp8 = (int8_t)(ext & 0xff);
    snprintf(idx_out, idx_cap, "%s%d.%s", is_addr ? "a" : "d", reg, is_long ? "l" : "w");
}

/* mode/reg per the standard 68000 effective-address field split (3+3 bits).
 * size only matters for mode 7 reg 4 (immediate). Advances *pc past any
 * extension words the addressing mode consumes. */
static void decode_ea(uint32_t *pc, int mode, int reg, int size, char *out, size_t cap)
{
    switch (mode)
    {
    case 0: snprintf(out, cap, "d%d", reg); return;
    case 1: snprintf(out, cap, "a%d", reg); return;
    case 2: snprintf(out, cap, "(a%d)", reg); return;
    case 3: snprintf(out, cap, "(a%d)+", reg); return;
    case 4: snprintf(out, cap, "-(a%d)", reg); return;
    case 5:
    {
        int16_t d = (int16_t)fetch16(pc);
        snprintf(out, cap, "%d(a%d)", d, reg);
        return;
    }
    case 6:
    {
        uint16_t ext = fetch16(pc);
        char idx[16];
        int disp8;
        decode_brief_ext(ext, idx, sizeof(idx), &disp8);
        snprintf(out, cap, "%d(a%d,%s)", disp8, reg, idx);
        return;
    }
    case 7:
        switch (reg)
        {
        case 0:
        {
            int16_t v = (int16_t)fetch16(pc);
            snprintf(out, cap, "$%x.w", (unsigned)(uint16_t)v);
            return;
        }
        case 1:
        {
            uint32_t v = fetch32(pc);
            snprintf(out, cap, "$%x.l", (unsigned)v);
            return;
        }
        case 2:
        {
            uint32_t ext_pc = *pc;
            int16_t d = (int16_t)fetch16(pc);
            snprintf(out, cap, "$%x(pc)", (unsigned)(ext_pc + (uint32_t)(int32_t)d));
            return;
        }
        case 3:
        {
            uint32_t ext_pc = *pc;
            uint16_t ext = fetch16(pc);
            char idx[16];
            int disp8;
            decode_brief_ext(ext, idx, sizeof(idx), &disp8);
            snprintf(out, cap, "$%x(pc,%s)", (unsigned)(ext_pc + (uint32_t)disp8), idx);
            return;
        }
        case 4:
            if (size == 2)
            {
                uint32_t v = fetch32(pc);
                snprintf(out, cap, "#$%x", (unsigned)v);
            }
            else
            {
                uint16_t v = fetch16(pc);
                snprintf(out, cap, "#$%x", (unsigned)(size == 0 ? (v & 0xff) : v));
            }
            return;
        default:
            snprintf(out, cap, "?");
            return;
        }
    default:
        snprintf(out, cap, "?");
        return;
    }
}

/* MOVEM's register mask is bit0=D0..bit7=D7,bit8=A0..bit15=A7 for every
 * mode except predecrement (-(An)), where the mask is reversed:
 * bit0=A7..bit15=D0 (68000 PRM 4-116). */
static void format_movem_list(uint16_t mask, int predecrement, char *out, size_t cap)
{
    size_t pos = 0;
    int i;
    int first = 1;
    out[0] = '\0';
    for (i = 0; i < 16 && pos < cap; i++)
    {
        int bit = predecrement ? (15 - i) : i;
        const char *name;
        char namebuf[4];
        if (!((mask >> bit) & 1))
            continue;
        if (bit < 8)
            snprintf(namebuf, sizeof(namebuf), "d%d", bit);
        else
            snprintf(namebuf, sizeof(namebuf), "a%d", bit - 8);
        name = namebuf;
        pos += (size_t)snprintf(out + pos, cap - pos, "%s%s", first ? "" : "/", name);
        first = 0;
    }
}

uint32_t m68k_disasm_one(uint32_t addr, char *out, size_t out_size)
{
    uint32_t pc = addr;
    uint16_t op = fetch16(&pc);
    char ea1[40], ea2[40];

    /* MOVE / MOVEA: top nibble picks size (1=byte,2=long,3=word); dest
     * field order is reg(11-9) then mode(8-6) - reversed vs. every other
     * EA field, which really is how this one instruction is encoded. */
    if ((op >> 12) == 1 || (op >> 12) == 2 || (op >> 12) == 3)
    {
        int size = (op >> 12) == 1 ? 0 : (op >> 12) == 2 ? 2 : 1;
        int src_mode = (op >> 3) & 7, src_reg = op & 7;
        int dst_mode = (op >> 6) & 7, dst_reg = (op >> 9) & 7;
        decode_ea(&pc, src_mode, src_reg, size, ea1, sizeof(ea1));
        decode_ea(&pc, dst_mode, dst_reg, size, ea2, sizeof(ea2));
        snprintf(out, out_size, "%s.%c %s,%s", dst_mode == 1 ? "movea" : "move",
                 size_letter(size), ea1, ea2);
        return pc - addr;
    }

    /* MOVEQ: 0111 rrr 0 dddddddd */
    if ((op & 0xF100) == 0x7000)
    {
        int reg = (op >> 9) & 7;
        int8_t data = (int8_t)(op & 0xff);
        snprintf(out, out_size, "moveq #%d,d%d", data, reg);
        return pc - addr;
    }

    /* Bcc/BRA/BSR: 0110 cccc dddddddd (8-bit disp, or 16-bit if disp8==0) */
    if ((op & 0xF000) == 0x6000)
    {
        int cc = (op >> 8) & 0xf;
        int8_t disp8 = (int8_t)(op & 0xff);
        uint32_t target;
        if (disp8 == 0)
        {
            int16_t disp16 = (int16_t)fetch16(&pc);
            target = addr + 2 + (uint32_t)(int32_t)disp16;
        }
        else
            target = addr + 2 + (uint32_t)(int32_t)disp8;
        if (cc == 0)
            snprintf(out, out_size, "bra $%x", (unsigned)target);
        else if (cc == 1)
            snprintf(out, out_size, "bsr $%x", (unsigned)target);
        else
            snprintf(out, out_size, "b%s $%x", CC_NAMES[cc], (unsigned)target);
        return pc - addr;
    }

    /* DBcc: 0101 cccc 11001 rrr, always followed by a 16-bit displacement */
    if ((op & 0xF0F8) == 0x50C8)
    {
        int cc = (op >> 8) & 0xf;
        int reg = op & 7;
        int16_t disp = (int16_t)fetch16(&pc);
        uint32_t target = addr + 2 + (uint32_t)(int32_t)disp;
        snprintf(out, out_size, "db%s d%d,$%x", CC_NAMES[cc], reg, (unsigned)target);
        return pc - addr;
    }

    /* ADDQ/SUBQ: 0101 ddd 0 SS mmmrrr (bit8=0 distinguishes from Scc/DBcc) */
    if ((op & 0xF100) == 0x5000)
    {
        int data = (op >> 9) & 7;
        if (data == 0) data = 8;
        int size = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (size <= 2)
        {
            int is_sub = (op >> 8) & 1;
            decode_ea(&pc, mode, reg, size, ea1, sizeof(ea1));
            snprintf(out, out_size, "%s.%c #%d,%s", is_sub ? "subq" : "addq",
                     size_letter(size), data, ea1);
            return pc - addr;
        }
    }

    /* LEA: 0100 rrr 111 mmmrrr (control addressing modes only) */
    if ((op & 0xF1C0) == 0x41C0)
    {
        int reg = (op >> 9) & 7;
        int mode = (op >> 3) & 7, ea_reg = op & 7;
        decode_ea(&pc, mode, ea_reg, 2, ea1, sizeof(ea1));
        snprintf(out, out_size, "lea %s,a%d", ea1, reg);
        return pc - addr;
    }

    /* PEA vs SWAP share the 0x4840-0x487F range; SWAP is register-direct
     * mode (000), PEA is every control addressing mode otherwise. */
    if ((op & 0xFFC0) == 0x4840)
    {
        int mode = (op >> 3) & 7, reg = op & 7;
        if (mode == 0)
            snprintf(out, out_size, "swap d%d", reg);
        else
        {
            decode_ea(&pc, mode, reg, 2, ea1, sizeof(ea1));
            snprintf(out, out_size, "pea %s", ea1);
        }
        return pc - addr;
    }

    /* EXT: 0100 100 0 1 s 000 rrr (s: 0=byte->word, 1=word->long) */
    if ((op & 0xFEB8) == 0x4880 && ((op >> 3) & 7) == 0)
    {
        int reg = op & 7;
        int is_long = (op >> 6) & 1;
        snprintf(out, out_size, "ext.%s d%d", is_long ? "l" : "w", reg);
        return pc - addr;
    }

    /* CLR/NOT/NEG/NEGX: 0100 oooo SS mmmrrr, oooo in {0000,0100,0110,1010} */
    if ((op & 0xF900) == 0x4000 && ((op >> 6) & 3) != 3)
    {
        static const struct { uint16_t bits; const char *name; } ops[] = {
            { 0x4000, "negx" }, { 0x4400, "neg" }, { 0x4600, "not" }, { 0x4200, "clr" }
        };
        uint16_t base = op & 0xFF00;
        size_t i;
        for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        {
            if (base == ops[i].bits)
            {
                int size = (op >> 6) & 3;
                int mode = (op >> 3) & 7, reg = op & 7;
                decode_ea(&pc, mode, reg, size, ea1, sizeof(ea1));
                snprintf(out, out_size, "%s.%c %s", ops[i].name, size_letter(size), ea1);
                return pc - addr;
            }
        }
    }

    /* TAS (0100101011mmmrrr) vs TST (0100101 0 SS mmmrrr, SS != 11) */
    if ((op & 0xFFC0) == 0x4AC0)
    {
        int mode = (op >> 3) & 7, reg = op & 7;
        decode_ea(&pc, mode, reg, 0, ea1, sizeof(ea1));
        snprintf(out, out_size, "tas %s", ea1);
        return pc - addr;
    }
    if ((op & 0xFF00) == 0x4A00 && ((op >> 6) & 3) != 3)
    {
        int size = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        decode_ea(&pc, mode, reg, size, ea1, sizeof(ea1));
        snprintf(out, out_size, "tst.%c %s", size_letter(size), ea1);
        return pc - addr;
    }

    /* JMP/JSR: 0100 1110 1 j mmmrrr (j: 0=jsr,1=jmp) */
    if ((op & 0xFF80) == 0x4E80)
    {
        int is_jmp = (op >> 6) & 1;
        int mode = (op >> 3) & 7, reg = op & 7;
        decode_ea(&pc, mode, reg, 2, ea1, sizeof(ea1));
        snprintf(out, out_size, "%s %s", is_jmp ? "jmp" : "jsr", ea1);
        return pc - addr;
    }

    /* LINK: 0100 1110 0101 0 rrr, word displacement follows */
    if ((op & 0xFFF8) == 0x4E50)
    {
        int reg = op & 7;
        int16_t disp = (int16_t)fetch16(&pc);
        snprintf(out, out_size, "link a%d,#%d", reg, disp);
        return pc - addr;
    }
    /* UNLK: 0100 1110 0101 1 rrr */
    if ((op & 0xFFF8) == 0x4E58)
    {
        snprintf(out, out_size, "unlk a%d", op & 7);
        return pc - addr;
    }
    /* TRAP: 0100 1110 0100 vvvv */
    if ((op & 0xFFF0) == 0x4E40)
    {
        snprintf(out, out_size, "trap #%d", op & 0xf);
        return pc - addr;
    }

    /* Fixed no-operand opcodes in the 0x4Exx misc range. */
    switch (op)
    {
    case 0x4E71: snprintf(out, out_size, "nop"); return pc - addr;
    case 0x4E75: snprintf(out, out_size, "rts"); return pc - addr;
    case 0x4E73: snprintf(out, out_size, "rte"); return pc - addr;
    case 0x4E77: snprintf(out, out_size, "rtr"); return pc - addr;
    case 0x4E76: snprintf(out, out_size, "trapv"); return pc - addr;
    case 0x4AFC: snprintf(out, out_size, "illegal"); return pc - addr;
    default: break;
    }

    /* MOVEM: 0100 1 d 0 0 1 s mmmrrr, register mask follows */
    if ((op & 0xFB80) == 0x4880)
    {
        int dir = (op >> 10) & 1; /* 0: reg->mem, 1: mem->reg */
        int is_long = (op >> 6) & 1;
        int mode = (op >> 3) & 7, reg = op & 7;
        uint16_t mask = fetch16(&pc);
        char list[128];
        format_movem_list(mask, mode == 4 && dir == 0, list, sizeof(list));
        decode_ea(&pc, mode, reg, is_long ? 2 : 1, ea1, sizeof(ea1));
        if (dir)
            snprintf(out, out_size, "movem.%c %s,%s", is_long ? 'l' : 'w', ea1, list);
        else
            snprintf(out, out_size, "movem.%c %s,%s", is_long ? 'l' : 'w', list, ea1);
        return pc - addr;
    }

    /* EXG: 1100 rrr 1 oooooo rrr (data-data 01000, addr-addr 01001, data-addr 10001) */
    if ((op & 0xF130) == 0xC100 && ((op >> 3) & 0x1f) != 0)
    {
        int mode = (op >> 3) & 0x1f;
        int rx = (op >> 9) & 7, ry = op & 7;
        if (mode == 0x08)
            snprintf(out, out_size, "exg d%d,d%d", rx, ry);
        else if (mode == 0x09)
            snprintf(out, out_size, "exg a%d,a%d", rx, ry);
        else if (mode == 0x11)
            snprintf(out, out_size, "exg d%d,a%d", rx, ry);
        else
            snprintf(out, out_size, ".dw $%x", op);
        return pc - addr;
    }

    /* ADD/SUB/AND/OR/EOR/CMP register-direct-destination forms: top nibble
     * selects the op, bit8 (opmode bit2) selects Dn-dest vs EA-dest for
     * add/sub/and/or; eor and cmp have their own bit meanings but this
     * covers the common "<op>.sz <ea>,dN" case, which is most of what
     * shows up in practice. EA-dest and address-register forms (ADDA/SUBA/
     * CMPA, memory-destination ADD/SUB/AND/OR, ADDX/SUBX/CMPM, ABCD/SBCD,
     * MULU/MULS/DIVU/DIVS) fall through to the .dw fallback below. */
    {
        static const struct { int nibble; const char *name; } alu[] = {
            { 0x8, "or" }, { 0x9, "sub" }, { 0xB, "cmp" }, { 0xC, "and" },
            { 0xD, "add" }
        };
        size_t i;
        int nib = (op >> 12) & 0xf;
        for (i = 0; i < sizeof(alu) / sizeof(alu[0]); i++)
        {
            if (alu[i].nibble != nib)
                continue;
            {
                int reg = (op >> 9) & 7;
                int opmode = (op >> 6) & 7;
                int mode = (op >> 3) & 7, ea_reg = op & 7;
                /* opmode 0-2 is the common "<op>.sz ea,dN" form for all
                 * five of these; opmode 3/7 (ADDA/SUBA/CMPA, word/long)
                 * and 4-6 (memory-destination ADD/SUB/AND/OR) fall through
                 * to the .dw fallback below, except EOR's 4-6 case, which
                 * is handled separately right after this loop. */
                if (opmode <= 2)
                {
                    decode_ea(&pc, mode, ea_reg, opmode, ea1, sizeof(ea1));
                    snprintf(out, out_size, "%s.%c %s,d%d", alu[i].name, size_letter(opmode), ea1, reg);
                    return pc - addr;
                }
            }
        }
        /* EOR: 1011 rrr 1 SS mmmrrr (opmode 4-6 = Dn -> EA; distinct from
         * CMP's opmode 0-2 handled above, which shares nibble 0xB). */
        if (nib == 0xB && ((op >> 6) & 7) >= 4 && ((op >> 6) & 7) <= 6)
        {
            int reg = (op >> 9) & 7;
            int size = ((op >> 6) & 7) - 4;
            int mode = (op >> 3) & 7, ea_reg = op & 7;
            decode_ea(&pc, mode, ea_reg, size, ea1, sizeof(ea1));
            snprintf(out, out_size, "eor.%c d%d,%s", size_letter(size), reg, ea1);
            return pc - addr;
        }
    }

    /* Shift/rotate, register form: 1110 ccc d SS i oo rrr
     * (c=count/reg, d=dir, SS=size, i=immediate/register count, oo=op) */
    if ((op & 0xF000) == 0xE000 && ((op >> 6) & 3) != 3)
    {
        static const char *names[4] = { "as", "ls", "rox", "ro" };
        int cnt_or_reg = (op >> 9) & 7;
        int dir = (op >> 8) & 1;
        int size = (op >> 6) & 3;
        int use_reg = (op >> 5) & 1;
        int which = (op >> 3) & 3;
        int reg = op & 7;
        if (use_reg)
            snprintf(out, out_size, "%s%s.%c d%d,d%d", names[which], dir ? "l" : "r",
                     size_letter(size), cnt_or_reg, reg);
        else
        {
            int count = cnt_or_reg == 0 ? 8 : cnt_or_reg;
            snprintf(out, out_size, "%s%s.%c #%d,d%d", names[which], dir ? "l" : "r",
                     size_letter(size), count, reg);
        }
        return pc - addr;
    }

    /* Everything else (line-A, line-F, memory-destination ALU forms, ADDA/
     * SUBA/CMPA, ADDX/SUBX/CMPM, ABCD/SBCD, MULU/MULS/DIVU/DIVS, immediate-
     * to-EA ops, BTST/BCHG/BCLR/BSET, MOVEP, Scc, ANDI/EORI/ORI/CMPI to
     * CCR/SR, STOP/RESET) - not decoded, see this file's header comment. */
    snprintf(out, out_size, ".dw $%x", op);
    return pc - addr;
}
