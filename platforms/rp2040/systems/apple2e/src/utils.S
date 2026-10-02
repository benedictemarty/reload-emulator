#include "pico/config.h"
#include "hardware/regs/addressmap.h"
#include "hardware/regs/sio.h"

// Offsets suitable for ldr/str (must be <= 0x7c):
#define ACCUM0_OFFS     (SIO_INTERP0_ACCUM0_OFFSET    - SIO_INTERP0_ACCUM0_OFFSET)
#define ACCUM1_OFFS     (SIO_INTERP0_ACCUM1_OFFSET    - SIO_INTERP0_ACCUM0_OFFSET)
#define POP0_OFFS       (SIO_INTERP0_POP_LANE0_OFFSET - SIO_INTERP0_ACCUM0_OFFSET)

.syntax unified
.cpu cortex-m0plus
.thumb

.macro decl_func_x name
.section .scratch_x.\name, "ax"
.global \name
.type \name,%function
.thumb_func
\name:
.endm

.macro decl_func_y name
.section .scratch_y.\name, "ax"
.global \name
.type \name,%function
.thumb_func
\name:
.endm

#define decl_func decl_func_x

.macro decode_pixel_data rd
	ldr \rd, [r2, #POP0_OFFS]
	lsls \rd, #8
	ldr r7, [r2, #POP0_OFFS]
	orrs \rd, r7
	ldr r7, [r2, #POP0_OFFS]
	lsls r7, #24
	orrs \rd, r7
	ldr r7, [r2, #POP0_OFFS]
	lsls r7, #16
	orrs \rd, r7
.endm

decl_func apple2e_render_scanline
	push {r3-r7, lr}
	
	lsls r2, #1
	add r2, r1
	mov ip, r2
	ldr r2, =(SIO_BASE + SIO_INTERP0_ACCUM0_OFFSET)

.align 2
1:
	ldmia r0!, {r7}
	str r7, [r2, #ACCUM0_OFFS]
	str r7, [r2, #ACCUM1_OFFS]
	decode_pixel_data r3
	decode_pixel_data r4
    stmia r1!, {r3, r4}
	cmp r1, ip
    bne 1b

	pop {r3-r7, pc}

//
decl_func copy_tmdsbuf
	push {r2-r7, lr}

// One TMDS line: 3 channels x FRAME_WIDTH / 2 words, 6 words a step (200 for 800 pixels,
// 160 for 640: with 200 the 640x480 build overran its TMDS buffers by 240 words)
#ifdef APPLE_VIDEO_640
.rept 160
#else
.rept 200
#endif
	ldmia r1!, {r2, r3, r4, r5, r6, r7}
	stmia r0!, {r2, r3, r4, r5, r6, r7}
.endr

	pop {r2-r7, pc}

// DC balanced line encoding (apple2e.c, encode_line): two pixels a word per TMDS channel.
// r0: Apple frame buffer row, one byte = two pixels (palette index of the first pixel in
//     the high nibble, as apple2e_render_scanline orders them)
// r1: output word of channel 0 (blue); green at r1 + r3, red at r1 + 2 * r3
// r2: number of pixel pairs (bytes); r3: channel stride in bytes
// apple_sym_even / apple_sym_odd: 16 colours x {blue, green, red, pad} words, the odd
// symbols already shifted left by 10
decl_func apple_encode_pairs
	push {r4-r7, lr}
	mov r4, r8
	mov r5, r9
	mov r6, r10
	mov r7, r11
	push {r4-r7}
	add r2, r0              // End of the row
	mov r10, r3             // Green offset
	lsls r3, #1
	mov r11, r3             // Red offset
	ldr r4, =apple_sym_even
	mov r8, r4
	ldr r4, =apple_sym_odd
	mov r9, r4
	b 2f
1:
	ldrb r3, [r0]
	adds r0, #1
	lsrs r4, r3, #4         // Even (first) pixel: high nibble
	lsls r4, #4             // 16 bytes a colour
	add r4, r8
	lsls r5, r3, #28        // Odd pixel: low nibble
	lsrs r5, #24
	add r5, r9
	ldr r6, [r4, #0]
	ldr r7, [r5, #0]
	orrs r6, r7
	str r6, [r1, #0]
	ldr r6, [r4, #4]
	ldr r7, [r5, #4]
	orrs r6, r7
	mov r3, r10
	str r6, [r1, r3]
	ldr r6, [r4, #8]
	ldr r7, [r5, #8]
	orrs r6, r7
	mov r3, r11
	str r6, [r1, r3]
	adds r1, #4
2:
	cmp r0, r2
	bne 1b
	pop {r4-r7}
	mov r8, r4
	mov r9, r5
	mov r10, r6
	mov r11, r7
	pop {r4-r7, pc}
