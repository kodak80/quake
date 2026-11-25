/*
 * snd_atari_sb.c -- Sound Blaster output for ISA equipped Ataris (Raven060)
 *
 * There is no ISA DMA, so this works like the pmdoom Sound Blaster driver:
 * MFP Timer A interrupts at ~11kHz and each interrupt sends one sample to the
 * DSP with the "direct DAC" command (0x10).
 *
 * Quake mixes into the same 16-bit stereo ring buffer layout it uses for
 * Falcon DMA, so the existing mixer (including the 68k asm transfer routine)
 * is used unchanged. The interrupt handler mixes left+right down to 8-bit
 * unsigned mono on the fly and advances the play position.
 *
 * This file deliberately does not include quakedef.h: isa.h needs stdbool.h,
 * whose true/false macros clash with Quake's qboolean enum.
 *
 * Released under the GNU GPL v2 or later, like the rest of Atari Quake.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <mint/osbind.h>
#include <mint/cookie.h>

#define ISA_EXCLUDE_LIB_MSDOS
#include "isa.h"

#include "snd_atari_sb.h"

/* shared with the interrupt handler */
volatile uint8_t*  q_sb_dsp;        /* DSP write/status register */
volatile uint8_t*  q_sb_buf;        /* ring buffer, 16-bit stereo frames */
volatile uint32_t  q_sb_pos;        /* play position in bytes */
uint32_t           q_sb_mask;       /* buffer size in bytes - 1 */

static uint16_t sb_port;
static uint32_t sb_oldvec;
static uint8_t  sb_tactrl;
static uint8_t  sb_tadata;
static int      sb_freq;
static int      sb_active;

#define MFP_IERA    (*(volatile uint8_t*)0xFFFFFA07)
#define MFP_IMRA    (*(volatile uint8_t*)0xFFFFFA13)
#define MFP_VR      (*(volatile uint8_t*)0xFFFFFA17)
#define MFP_TACR    (*(volatile uint8_t*)0xFFFFFA19)
#define MFP_TADR    (*(volatile uint8_t*)0xFFFFFA1F)

#define Q_STR2(x) #x
#define Q_STR(x)  Q_STR2(x)
#define SYM(name) Q_STR(__USER_LABEL_PREFIX__) #name

/*
 * Timer A interrupt: one sample per call.
 *   sample = ((left + right) / 2) >> 8, converted to unsigned 8-bit
 */
__asm__(
"	.text\n"
"	.even\n"
"	.globl	" SYM(q_sb_timera) "\n"
SYM(q_sb_timera) ":\n"
"	movem.l	%d0-%d2/%a0-%a1,-(%sp)\n"
"	move.l	" SYM(q_sb_buf) ",%a1\n"
"	move.l	" SYM(q_sb_pos) ",%d0\n"
"	move.w	(%a1,%d0.l),%d1\n"
"	ext.l	%d1\n"
"	move.w	2(%a1,%d0.l),%d2\n"
"	ext.l	%d2\n"
"	add.l	%d2,%d1\n"
"	asr.l	#8,%d1\n"
"	asr.l	#1,%d1\n"
"	eor.b	#0x80,%d1\n"
"	move.l	" SYM(q_sb_dsp) ",%a0\n"
"	move.b	#0x10,(%a0)\n"            /* direct DAC command */
"1:	tst.b	(%a0)\n"                  /* wait while DSP busy (bit 7) */
"	bmi.s	1b\n"
"	move.b	%d1,(%a0)\n"
"	addq.l	#4,%d0\n"
"	and.l	" SYM(q_sb_mask) ",%d0\n"
"	move.l	%d0," SYM(q_sb_pos) "\n"
"	move.b	#0xdf,0xfffffa0f\n"       /* clear Timer A in-service bit */
"	movem.l	(%sp)+,%d0-%d2/%a0-%a1\n"
"	rte\n"
);

extern void q_sb_timera(void);

static uint16_t sr_disable(void)
{
	uint16_t sr;
	__asm__ volatile ("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "cc");
	return sr;
}

static void sr_restore(uint16_t sr)
{
	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "cc");
}

/* best MFP Timer A prescaler/data pair for hz, returns the real rate */
static int timera_calc(int hz, uint8_t* ctrl, uint8_t* data)
{
	static const uint32_t div[7] = { 4, 10, 16, 50, 64, 100, 200 };
	const uint32_t clock = 2457600;
	int best_hz = 0, best_diff = 0x7fffffff;
	int i, j;

	*ctrl = 1;
	*data = 1;
	for (i = 7; i != 0; i--) {
		uint32_t base = clock / div[i - 1];
		for (j = 1; j < 256; j++) {
			int val = base / j;
			int diff = (hz > val) ? (hz - val) : (val - hz);
			if (diff < best_diff) {
				best_hz = val;
				best_diff = diff;
				*ctrl = i;
				*data = j;
			}
			if (val < hz)
				break;
		}
	}
	return best_hz;
}

static void dsp_write(uint8_t data)
{
	while (isa_readb(sb_port + 0xC) & 0x80) { }
	isa_writeb(sb_port + 0xC, data);
}

static uint16_t sb_detect(void)
{
	uint16_t port;
	for (port = 0x220; port <= 0x280; port += 0x20) {
		uint8_t old = isa_readb(port + 0x6);
		isa_writeb(port + 0x6, 1); isa_delay_super(3000);
		isa_writeb(port + 0x6, 0); isa_delay_super(3000);
		if (isa_readb(port + 0xA) == 0xAA)
			return port;
		isa_writeb(port + 0x6, old); isa_delay_super(3000);
	}
	return 0;
}

static long sb_start_super(void)
{
	uint16_t sr;
	uint32_t vecp;

	if (!isa_init())
		return 0;
	sb_port = sb_detect();
	if (sb_port == 0)
		return 0;

	q_sb_dsp = (volatile uint8_t*)(isa_if->iobase + sb_port + 0xC);
	dsp_write(0xD1);                        /* speaker on */

	sb_freq = timera_calc(sb_freq, &sb_tactrl, &sb_tadata);

	sr = sr_disable();
	MFP_TACR = 0;
	MFP_TADR = sb_tadata;
	vecp = ((MFP_VR & 0xF0) + 13) * 4;      /* Timer A vector */
	sb_oldvec = *(volatile uint32_t*)vecp;
	*(volatile uint32_t*)vecp = (uint32_t)q_sb_timera;
	MFP_IERA |= (1 << 5);
	MFP_IMRA |= (1 << 5);
	MFP_TACR = sb_tactrl;
	sr_restore(sr);
	return 1;
}

static long sb_stop_super(void)
{
	uint16_t sr = sr_disable();
	uint32_t vecp = ((MFP_VR & 0xF0) + 13) * 4;

	MFP_TACR = 0;
	MFP_IERA &= ~(1 << 5);
	MFP_IMRA &= ~(1 << 5);
	*(volatile uint32_t*)vecp = sb_oldvec;
	sr_restore(sr);

	dsp_write(0xD3);                        /* speaker off */
	return 1;
}

int atari_sb_init(void* buffer, int bytes, int* freq)
{
	if (sb_active)
		return sb_port;

	q_sb_buf  = (volatile uint8_t*)buffer;
	q_sb_mask = (uint32_t)bytes - 1;        /* bytes must be a power of 2 */
	q_sb_pos  = 0;
	sb_freq   = *freq;

	if (!Supexec(sb_start_super))
		return 0;

	*freq = sb_freq;
	sb_active = 1;
	return sb_port;
}

int atari_sb_getpos(void)
{
	return (int)q_sb_pos;
}

void atari_sb_shutdown(void)
{
	if (!sb_active)
		return;
	Supexec(sb_stop_super);
	sb_active = 0;
}
