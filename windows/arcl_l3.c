/* L3 tools: arcl_video, arcl_palette, arcl_sprites, arcl_dma, arcl_irq,
 * arcl_vram (x68k_mcp.md 6.4). All read-only, all decode existing exported
 * core globals (crtc.h/palette.h/bg.h/dmac.h/mfp.h/ioc.h/gvram.h/tvram.h) -
 * no core modification. Where a register's bit layout wasn't confidently
 * derivable from reading the core's own code in the time available, the
 * raw register is exposed alongside whatever *is* decoded, same
 * documented-reduced-scope pattern as arcl_disasm (windows/m68k_disasm.c). */
#include "arcl_l3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arcl_opm.h"
#include "mcp_json.h"

#include "bg.h"
#include "crtc.h"
#include "dmac.h"
#include "gvram.h"
#include "ioc.h"
#include "mfp.h"
#include "palette.h"
#include "tvram.h"

struct arcl_l3 {
    arcl_control_t *control;
};

#define ARCL_VRAM_READ_MAX 65536

static void append_hex_bytes(char *out, size_t cap, size_t *used, const uint8_t *data, size_t len)
{
    size_t i;
    mcp_json_appendf(out, cap, used, "\"");
    for (i = 0; i < len; i++)
        mcp_json_appendf(out, cap, used, "%02x", data[i]);
    mcp_json_appendf(out, cap, used, "\"");
}

static int handle_video(arcl_l3_t *l3, char *result_json, size_t result_size)
{
    size_t used = 0;
    int i;
    /* Reads core globals (CRTC_Regs, TextDotX/Y, ...) directly - not
     * self-synchronized against arcl_resume, unlike arcl_opm_shadow_get(). */
    arcl_control_lock(l3->control);
    mcp_json_appendf(result_json, result_size, &used,
        "{\"text\":{\"dot_x\":%u,\"dot_y\":%u,\"scroll_x\":%u,\"scroll_y\":%u},"
        "\"graphic\":{\"scroll\":[",
        (unsigned)TextDotX, (unsigned)TextDotY, (unsigned)TextScrollX, (unsigned)TextScrollY);
    for (i = 0; i < 4; i++)
        mcp_json_appendf(result_json, result_size, &used,
                          "%s{\"page\":%d,\"x\":%u,\"y\":%u}",
                          i ? "," : "", i, (unsigned)GrphScrollX[i], (unsigned)GrphScrollY[i]);
    mcp_json_appendf(result_json, result_size, &used,
        "]},\"crtc\":{\"vstart\":%u,\"vend\":%u,\"hstart\":%u,\"hend\":%u,\"mode\":%u,\"int_line\":%u,\"regs\":",
        (unsigned)CRTC_VSTART, (unsigned)CRTC_VEND, (unsigned)CRTC_HSTART, (unsigned)CRTC_HEND,
        (unsigned)CRTC_Mode, (unsigned)CRTC_IntLine);
    append_hex_bytes(result_json, result_size, &used, CRTC_Regs, sizeof(CRTC_Regs));
    mcp_json_appendf(result_json, result_size, &used, "},\"vcreg\":{\"reg0\":");
    append_hex_bytes(result_json, result_size, &used, VCReg0, sizeof(VCReg0));
    mcp_json_appendf(result_json, result_size, &used, ",\"reg1\":");
    append_hex_bytes(result_json, result_size, &used, VCReg1, sizeof(VCReg1));
    mcp_json_appendf(result_json, result_size, &used, ",\"reg2\":");
    append_hex_bytes(result_json, result_size, &used, VCReg2, sizeof(VCReg2));
    mcp_json_appendf(result_json, result_size, &used, "}");
    arcl_control_unlock(l3->control);
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

/* X68000 palette registers are 16-bit GGGGGRRRRRBBBBBI (5 green / 5 red /
 * 5 blue / 1 intensity, MSB first) - confirmed by reading Pal_SetColor()'s
 * bit-unpack loop in x68k/palette.c. Expanded to 8-bit/channel with the
 * standard 5->8 bit replication (v8 = (v5<<3)|(v5>>2)); the hardware's
 * intensity-bit half-brightness behavior itself isn't modeled, so
 * `intensity` is reported raw rather than folded into r/g/b. */
static void decode_pal_entry(const uint8_t *regs, int index, int *r, int *g, int *b, int *intensity)
{
    uint16_t v = ((uint16_t)regs[index * 2] << 8) | regs[index * 2 + 1];
    int r5 = (v >> 6) & 0x1f, g5 = (v >> 11) & 0x1f, b5 = (v >> 1) & 0x1f;
    *r = (r5 << 3) | (r5 >> 2);
    *g = (g5 << 3) | (g5 >> 2);
    *b = (b5 << 3) | (b5 >> 2);
    *intensity = v & 1;
}

static void append_palette_array(char *out, size_t cap, size_t *used, const uint8_t *base)
{
    int i;
    mcp_json_appendf(out, cap, used, "[");
    for (i = 0; i < 256; i++)
    {
        int r, g, b, intensity;
        decode_pal_entry(base, i, &r, &g, &b, &intensity);
        mcp_json_appendf(out, cap, used,
                          "%s{\"index\":%d,\"r\":%d,\"g\":%d,\"b\":%d,\"intensity\":%s}",
                          i ? "," : "", i, r, g, b, intensity ? "true" : "false");
    }
    mcp_json_appendf(out, cap, used, "]");
}

static int handle_palette(arcl_l3_t *l3, char *result_json, size_t result_size)
{
    size_t used = 0;
    mcp_json_appendf(result_json, result_size, &used, "{\"graphic\":");
    arcl_control_lock(l3->control); /* reads Pal_Regs[] directly, not self-synchronized against arcl_resume */
    /* Pal_Write: adr<0x200 -> graphic palette, 0x200<=adr<0x400 -> text palette. */
    append_palette_array(result_json, result_size, &used, Pal_Regs);
    mcp_json_appendf(result_json, result_size, &used, ",\"text\":");
    append_palette_array(result_json, result_size, &used, Pal_Regs + 0x200);
    arcl_control_unlock(l3->control);
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

/* Sprite_Regs[] holds 128 fixed-format 8-byte hardware sprite entries
 * (struct SPRITECTRLTBL in x68k/bg.c: posx u16, posy u16, ctrl u16, ply
 * u8, dummy u8). ctrl's low bits are the pattern number and its bit14/15
 * encode H/V reverse in a form not confidently reverse-engineered from the
 * renderer in the time available (see Sprite_DrawLineMcr) - so ctrl is
 * also reported raw. (ctrl>>4)&0xf is the palette bank (used directly by
 * the renderer to OR into the pixel's palette index), and ply&3 is
 * priority (also used directly by the renderer for draw ordering). */
static int handle_sprites(arcl_l3_t *l3, char *result_json, size_t result_size)
{
    extern uint8_t Sprite_Regs[0x800];
    size_t used = 0;
    int i;
    mcp_json_appendf(result_json, result_size, &used, "{\"sprites\":[");
    arcl_control_lock(l3->control); /* reads Sprite_Regs[]/BG_Regs[] directly, not self-synchronized against arcl_resume */
    for (i = 0; i < 128; i++)
    {
        const uint8_t *e = Sprite_Regs + i * 8;
        uint16_t posx = (uint16_t)(e[0] | (e[1] << 8));
        uint16_t posy = (uint16_t)(e[2] | (e[3] << 8));
        uint16_t ctrl = (uint16_t)(e[4] | (e[5] << 8));
        uint8_t ply = e[6];
        mcp_json_appendf(result_json, result_size, &used,
            "%s{\"index\":%d,\"x\":%u,\"y\":%u,\"priority\":%u,\"palette\":%u,\"ctrl_raw\":\"0x%x\"}",
            i ? "," : "", i, posx, posy, ply & 3, (ctrl >> 4) & 0xf, ctrl);
    }
    mcp_json_appendf(result_json, result_size, &used,
        "],\"bg_hadjust\":%d,\"bg_vline\":%d,\"vlinebg\":%u,\"bg_regs\":",
        (int)BG_HAdjust, (int)BG_VLINE, (unsigned)VLINEBG);
    append_hex_bytes(result_json, result_size, &used, BG_Regs, sizeof(BG_Regs));
    arcl_control_unlock(l3->control);
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

/* MC68450 DMAC, standard register set (x68k/dmac.h's dmac_ch). Named
 * fields use the datasheet's own register names (CSR/CER/DCR/... are
 * documented MC68450 abbreviations, not something guessed for this tool). */
static int handle_dma(arcl_l3_t *l3, char *result_json, size_t result_size)
{
    size_t used = 0;
    int i;
    mcp_json_appendf(result_json, result_size, &used, "{\"channels\":[");
    arcl_control_lock(l3->control); /* reads DMA[] directly, not self-synchronized against arcl_resume */
    for (i = 0; i < 4; i++)
    {
        dmac_ch *c = &DMA[i];
        mcp_json_appendf(result_json, result_size, &used,
            "%s{\"ch\":%d,\"csr\":\"0x%x\",\"cer\":\"0x%x\",\"dcr\":\"0x%x\",\"ocr\":\"0x%x\","
            "\"scr\":\"0x%x\",\"ccr\":\"0x%x\",\"mtc\":%u,\"mar\":\"0x%x\",\"dar\":\"0x%x\","
            "\"btc\":%u,\"bar\":\"0x%x\",\"niv\":\"0x%x\",\"eiv\":\"0x%x\",\"mfc\":\"0x%x\","
            "\"cpr\":\"0x%x\",\"dfc\":\"0x%x\",\"bfc\":\"0x%x\",\"gcr\":\"0x%x\"}",
            i ? "," : "", i, c->CSR, c->CER, c->DCR, c->OCR, c->SCR, c->CCR,
            c->MTC, (unsigned)c->MAR, (unsigned)c->DAR, c->BTC, (unsigned)c->BAR,
            c->NIV, c->EIV, c->MFC, c->CPR, c->DFC, c->BFC, c->GCR);
    }
    arcl_control_unlock(l3->control);
    mcp_json_appendf(result_json, result_size, &used, "]");
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

/* MC68901 MFP, standard register set (x68k/mfp.h's MFP_* indices into the
 * 24-byte MFP[] array) - the X68000's primary interrupt controller (FDC,
 * keyboard, timers, etc. all route through it). IOC_IntStat/IntVect
 * (x68k/ioc.h) are the separate I/O controller's own interrupt status. */
static int handle_irq(arcl_l3_t *l3, char *result_json, size_t result_size)
{
    size_t used;
    /* Reads MFP[]/IOC_IntStat/IOC_IntVect/CRTC_IntLine directly, not
     * self-synchronized against arcl_resume. */
    arcl_control_lock(l3->control);
    used = (size_t)snprintf(result_json, result_size,
        "{\"mfp\":{\"gpip\":\"0x%x\",\"aer\":\"0x%x\",\"ddr\":\"0x%x\","
        "\"iera\":\"0x%x\",\"ierb\":\"0x%x\",\"ipra\":\"0x%x\",\"iprb\":\"0x%x\","
        "\"isra\":\"0x%x\",\"isrb\":\"0x%x\",\"imra\":\"0x%x\",\"imrb\":\"0x%x\",\"vr\":\"0x%x\","
        "\"tacr\":\"0x%x\",\"tbcr\":\"0x%x\",\"tcdcr\":\"0x%x\","
        "\"tadr\":\"0x%x\",\"tbdr\":\"0x%x\",\"tcdr\":\"0x%x\",\"tddr\":\"0x%x\","
        "\"scr\":\"0x%x\",\"ucr\":\"0x%x\",\"rsr\":\"0x%x\",\"tsr\":\"0x%x\",\"udr\":\"0x%x\"},"
        "\"ioc\":{\"int_stat\":\"0x%x\",\"int_vect\":\"0x%x\"},\"crtc_int_line\":%u",
        MFP[MFP_GPIP], MFP[MFP_AER], MFP[MFP_DDR], MFP[MFP_IERA], MFP[MFP_IERB],
        MFP[MFP_IPRA], MFP[MFP_IPRB], MFP[MFP_ISRA], MFP[MFP_ISRB], MFP[MFP_IMRA], MFP[MFP_IMRB],
        MFP[MFP_VR], MFP[MFP_TACR], MFP[MFP_TBCR], MFP[MFP_TCDCR],
        MFP[MFP_TADR], MFP[MFP_TBDR], MFP[MFP_TCDR], MFP[MFP_TDDR],
        MFP[MFP_SCR], MFP[MFP_UCR], MFP[MFP_RSR], MFP[MFP_TSR], MFP[MFP_UDR],
        IOC_IntStat, IOC_IntVect, (unsigned)CRTC_IntLine);
    arcl_control_unlock(l3->control);
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

static int handle_vram(arcl_l3_t *l3, const char *request_json,
                        char *result_json, size_t result_size,
                        char *error_message, size_t error_size)
{
    char region[16];
    uint32_t address;
    long length;
    const uint8_t *base;
    size_t region_size;
    char *hexbuf;
    size_t i, used;

    if (!mcp_json_get_string_any(request_json, "region", region, sizeof(region)))
    {
        mcp_json_set_error(error_message, error_size, "missing required string field 'region' (gvram/tvram)");
        return 0;
    }
    if (strcmp(region, "gvram") == 0) { base = GVRAM; region_size = sizeof(GVRAM); }
    else if (strcmp(region, "tvram") == 0) { base = TVRAM; region_size = sizeof(TVRAM); }
    else
    {
        mcp_json_set_error(error_message, error_size, "region must be gvram or tvram");
        return 0;
    }
    if (!mcp_json_get_hex_or_int_any(request_json, "address", &address))
        address = 0;
    if (!mcp_json_get_long_any(request_json, "length", &length))
        length = 256;
    if (length < 1 || length > ARCL_VRAM_READ_MAX)
    {
        mcp_json_set_error(error_message, error_size, "length must be between 1 and 65536");
        return 0;
    }
    if (address >= region_size || (size_t)length > region_size - address)
    {
        mcp_json_set_error(error_message, error_size, "address/length out of range for this region (0x80000 bytes each)");
        return 0;
    }

    hexbuf = (char *)malloc((size_t)length * 2 + 1);
    if (!hexbuf)
    {
        mcp_json_set_error(error_message, error_size, "out of memory");
        return 0;
    }
    arcl_control_lock(l3->control); /* reads GVRAM[]/TVRAM[] directly, not self-synchronized against arcl_resume */
    for (i = 0; i < (size_t)length; i++)
        snprintf(hexbuf + i * 2, 3, "%02x", base[address + i]);
    arcl_control_unlock(l3->control);
    used = (size_t)snprintf(result_json, result_size,
                             "{\"region\":\"%s\",\"address\":\"0x%x\",\"length\":%ld,\"data\":\"%s\"",
                             region, (unsigned)address, length, hexbuf);
    free(hexbuf);
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

/* x68k_opm (machine-specific, arcl_common_spec.md 7.1/x68k_mcp.md 6.4):
 * decodes the YM2151 OPM's write-only register file via the shadow
 * fmg_wrap.cpp now maintains (windows/arcl_opm.c, hooked in under
 * ARCL_OPM_SHADOW - x68k_mcp.md 0.1.1). Field layout is the documented
 * YM2151 register map; reg 0x14's finer timer-control sub-bits (beyond
 * CSM) aren't decoded with confidence, so reg14_raw is given alongside.
 * CAVEAT (must stay in the tool description too): this shadow is separate
 * project state, not part of the core's own save-state blob, so it goes
 * stale (reads as all-zero) immediately after an arcl_load_state/
 * arcl_snapshot restore until the guest writes to OPM again.
 *
 * Sound-effect synchronization: `channels[].last_key_on_frame` and the
 * top-level `recent_key_on_events` give this response a time axis it
 * otherwise wouldn't have. Register 0x08 (key on/off) is the only
 * meaningfully edge-triggered part of the OPM's state - a short sound
 * effect can key a channel on and back off entirely within the frames a
 * single arcl_run call advances, and would otherwise be invisible by the
 * time this tool is polled (the raw key_on_mask snapshot below would just
 * show whatever the *next* write left behind, indistinguishable from "no
 * effect fired"). arcl_opm_shadow_on_frame_hint() (registered below,
 * called from arcl_control.c's advance_frame()) is what lets arcl_opm.c
 * attach a frame number to each such write in the first place. */
static int handle_opm(arcl_l3_t *l3, char *result_json, size_t result_size)
{
    uint8_t regs[256];
    uint8_t key_on[8];
    unsigned long long key_on_frame[8];
    arcl_opm_event_t events[ARCL_OPM_EVENT_LOG_MAX];
    size_t event_count, ei;
    size_t used = 0;
    int ch, slot;

    arcl_opm_shadow_get(regs, key_on);
    arcl_opm_shadow_get_key_on_frames(key_on_frame);
    event_count = arcl_opm_shadow_get_key_on_events(events);

    mcp_json_appendf(result_json, result_size, &used,
        "{\"lfo\":{\"freq\":\"0x%x\",\"pmd_amd_select\":\"%s\",\"pmd_amd_depth\":%u,"
        "\"waveform\":%u,\"ct1\":%s,\"ct2\":%s},"
        "\"noise\":{\"enable\":%s,\"freq\":%u},"
        "\"timer_a\":{\"value\":%u},\"timer_b\":{\"value\":%u},"
        "\"reg14_raw\":\"0x%x\",\"csm\":%s,\"channels\":[",
        regs[0x18],
        (regs[0x19] & 0x80) ? "pmd" : "amd", regs[0x19] & 0x7f,
        regs[0x1b] & 3, (regs[0x1b] & 0x80) ? "true" : "false", (regs[0x1b] & 0x40) ? "true" : "false",
        (regs[0x0f] & 0x80) ? "true" : "false", regs[0x0f] & 0x1f,
        (unsigned)(((unsigned)regs[0x10] << 2) | (regs[0x11] & 3)), regs[0x12],
        regs[0x14], (regs[0x14] & 0x80) ? "true" : "false");

    for (ch = 0; ch < 8; ch++)
    {
        uint8_t r20 = regs[0x20 + ch], r28 = regs[0x28 + ch], r30 = regs[0x30 + ch], r38 = regs[0x38 + ch];
        mcp_json_appendf(result_json, result_size, &used,
            "%s{\"ch\":%d,\"key_on_mask\":%u,\"last_key_on_frame\":%llu,\"rl\":%u,\"fb\":%u,\"connect\":%u,"
            "\"kc_octave\":%u,\"kc_note\":%u,\"kf\":%u,\"pms\":%u,\"ams\":%u,\"operators\":[",
            ch ? "," : "", ch, key_on[ch], key_on_frame[ch], (r20 >> 6) & 3, (r20 >> 3) & 7, r20 & 7,
            (r28 >> 4) & 7, r28 & 0xf, (r30 >> 2) & 0x3f, (r38 >> 4) & 7, r38 & 3);
        for (slot = 0; slot < 4; slot++)
        {
            int base = slot * 8 + ch;
            uint8_t dtmul = regs[0x40 + base], tl = regs[0x60 + base], ksar = regs[0x80 + base],
                    amsd1r = regs[0xa0 + base], dt2d2r = regs[0xc0 + base], d1lrr = regs[0xe0 + base];
            mcp_json_appendf(result_json, result_size, &used,
                "%s{\"slot\":%d,\"dt1\":%u,\"mul\":%u,\"tl\":%u,\"ks\":%u,\"ar\":%u,"
                "\"ams_en\":%s,\"d1r\":%u,\"dt2\":%u,\"d2r\":%u,\"d1l\":%u,\"rr\":%u}",
                slot ? "," : "", slot, (dtmul >> 4) & 7, dtmul & 0xf, tl & 0x7f, (ksar >> 6) & 3, ksar & 0x1f,
                (amsd1r & 0x80) ? "true" : "false", amsd1r & 0x1f, (dt2d2r >> 6) & 3, dt2d2r & 0x1f,
                (d1lrr >> 4) & 0xf, d1lrr & 0x1f);
        }
        mcp_json_appendf(result_json, result_size, &used, "]}");
    }
    mcp_json_appendf(result_json, result_size, &used, "],\"recent_key_on_events\":[");
    for (ei = 0; ei < event_count; ei++)
        mcp_json_appendf(result_json, result_size, &used,
            "%s{\"frame\":%llu,\"ch\":%u,\"key_on_mask\":%u}",
            ei ? "," : "", events[ei].frame, events[ei].channel, events[ei].key_on_mask);
    mcp_json_appendf(result_json, result_size, &used, "]");
    arcl_control_append_status(l3->control, result_json, result_size, used, -1);
    return 1;
}

/* arcl_opm_shadow_init()/_shutdown() are NOT called here - see the matching
 * comment on arcl_l2_init() in arcl_l2.c: the OPM write hook fires from
 * the core unconditionally, in interactive launches too, so its lifecycle
 * is owned by main(), not by whether L3 tools happen to be reachable. By
 * the time this runs (MCP startup, after main()'s early
 * arcl_opm_shadow_init()), the shadow already exists, so it's safe to hand
 * it a frame-hint sink registration here even though this module doesn't
 * own its init/shutdown. */
int arcl_l3_init(arcl_l3_t **out_l3, arcl_control_t *control)
{
    arcl_l3_t *l3;
    if (!out_l3 || !control)
        return 0;
    l3 = (arcl_l3_t *)calloc(1, sizeof(*l3));
    if (!l3)
        return 0;
    l3->control = control;
    arcl_control_set_frame_hint_sink(control, arcl_opm_shadow_on_frame_hint, NULL);
    *out_l3 = l3;
    return 1;
}

void arcl_l3_shutdown(arcl_l3_t *l3)
{
    free(l3);
}

int arcl_l3_call(const char *name, const char *request_json, void *userdata,
                  char *result_json, size_t result_size,
                  char *error_message, size_t error_size)
{
    arcl_l3_t *l3 = (arcl_l3_t *)userdata;
    if (!l3 || !name || !result_json || !error_message)
        return 0;
    error_message[0] = '\0';

    if (strcmp(name, "arcl_video") == 0)
        return handle_video(l3, result_json, result_size);
    if (strcmp(name, "arcl_palette") == 0)
        return handle_palette(l3, result_json, result_size);
    if (strcmp(name, "arcl_sprites") == 0)
        return handle_sprites(l3, result_json, result_size);
    if (strcmp(name, "arcl_dma") == 0)
        return handle_dma(l3, result_json, result_size);
    if (strcmp(name, "arcl_irq") == 0)
        return handle_irq(l3, result_json, result_size);
    if (strcmp(name, "arcl_vram") == 0)
        return handle_vram(l3, request_json, result_json, result_size, error_message, error_size);
    if (strcmp(name, "x68k_opm") == 0)
        return handle_opm(l3, result_json, result_size);

    mcp_json_set_error(error_message, error_size, "Unknown tool");
    return 0;
}
