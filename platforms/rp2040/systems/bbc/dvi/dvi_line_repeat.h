// Lines of output per TMDS buffer for the BBC (included by PicoDVI's dvi.c, see
// patches/picodvi-line-repeat.sh). One buffer per BBC line: at 800x480 the 256 BBC
// lines need 512 output lines, so the last line of every 8-line character row is
// shown once: 7 x 2 + 1 = 15 output lines per row, 32 x 15 = 480. 960x544 shows
// every line twice (PicoDVI's default with DVI_VERTICAL_REPEAT = 2).
#ifndef BBC_VIDEO_544
// v % 15 without a division (v < 480: v * 2185 >> 15 == v / 15), odd or 14 = last output line
static inline bool bbc_dvi_last_repeat(uint v) {
    uint q = v - ((v * 2185u) >> 15) * 15u;
    return (q & 1u) || q == 14u;
}
#define DVI_LAST_REPEAT(v) bbc_dvi_last_repeat(v)
#endif
