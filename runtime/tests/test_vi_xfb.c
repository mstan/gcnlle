#include "vi/vi.h"

#include <assert.h>
#include <stdio.h>

/* vi.c's field scheduler owns these observability/presentation call sites.
 * XFB-geometry tests never advance the beam, so inert definitions keep the
 * unit focused without pulling the complete GX/window runtime into the test. */
void gcn_ring_event(u16 kind, u32 detail, u32 aux, u32 pc) {
    (void)kind; (void)detail; (void)aux; (void)pc;
}
int gcn_host_window_enabled(void) { return 0; }
void gcn_host_window_present(const u8* xfb, u32 width, u32 height, u32 stride) {
    (void)xfb; (void)width; (void)height; (void)stride;
}
void gcn_gx_xfb_read_begin(void) {}
void gcn_gx_xfb_read_end(void) {}
u64 gcn_gx_xfb_generation(void) { return 0; }

static void set_u32(GcnVi* vi, u32 offset, u32 value) {
    vi->reg[offset >> 1] = (u16)(value >> 16);
    vi->reg[(offset + 2u) >> 1] = (u16)value;
}

static void expect_info(u32 expected_addr, u32 expected_width,
                        u32 expected_height, u32 expected_stride) {
    u32 addr, width, height, stride;
    assert(gcn_vi_xfb_info(&addr, &width, &height, &stride));
    assert(addr == expected_addr);
    assert(width == expected_width);
    assert(height == expected_height);
    assert(stride == expected_stride);
}

int main(void) {
    GcnVi vi;
    gcn_vi_init(&vi);

    const u32 base = 0x004C8EC0u;
    const u32 line_stride = 640u * 2u;
    vi.reg[GCN_VI_VTR >> 1] = 0x0F06u;       /* EQU=6, ACV=240 */
    vi.reg[GCN_VI_VTO_HI >> 1] = 5u;
    vi.reg[GCN_VI_VTO_LO >> 1] = 502u;
    vi.reg[GCN_VI_VTE_HI >> 1] = 4u;
    vi.reg[GCN_VI_VTE_LO >> 1] = 503u;
    vi.reg[0x48u >> 1] = 0x2850u;            /* WPL=40, STD=80 */
    set_u32(&vi, GCN_VI_FB_LEFT_TOP_HI, base);
    set_u32(&vi, GCN_VI_FB_LEFT_BOTTOM_HI, base + line_stride);

    /* Wind Waker's ordinary interlaced layout: reconstruct both 240-line
     * fields as the 640x480 XFB Dolphin presents by default. */
    expect_info(base, 640u, 480u, line_stride);

    /* A non-interlaced picture configuration retains literal field geometry. */
    vi.reg[0x48u >> 1] = 0x2828u;            /* WPL=40, STD=40 */
    expect_info(base, 640u, 240u, line_stride);

    /* The opposite PRB ordering makes top the second row. */
    vi.reg[0x48u >> 1] = 0x2850u;
    vi.reg[GCN_VI_VTO_LO >> 1] = 503u;
    vi.reg[GCN_VI_VTE_LO >> 1] = 502u;
    set_u32(&vi, GCN_VI_FB_LEFT_TOP_HI, base + line_stride);
    set_u32(&vi, GCN_VI_FB_LEFT_BOTTOM_HI, base);
    expect_info(base, 640u, 480u, line_stride);

    /* POFF is shared by both field-base registers. */
    vi.reg[GCN_VI_VTO_LO >> 1] = 502u;
    vi.reg[GCN_VI_VTE_LO >> 1] = 503u;
    set_u32(&vi, GCN_VI_FB_LEFT_TOP_HI,
            0x10000000u | (base >> 5));
    set_u32(&vi, GCN_VI_FB_LEFT_BOTTOM_HI,
            (base + line_stride) >> 5);
    expect_info(base, 640u, 480u, line_stride);

    puts("vi xfb geometry: ok");
    return 0;
}
