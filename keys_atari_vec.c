/*
 * keys_atari_vec.c -- keyboard and mouse input through the TOS 2.x+
 *                     kbdvec / mousevec hooks (Raven060, EmuTOS, clones)
 *
 * The original Falcon input (keys_atari_asm.s) replaces the ikbdsys vector and
 * reads the IKBD ACIA at $fffffc02 directly. That never runs on machines that
 * have no real ACIA: on the Raven060, EmuTOS takes the IKBD bytes from a UART
 * interrupt and feeds them straight into its packet parser, so ikbdsys is never
 * called.
 *
 * Instead this hooks the two vectors every TOS >= 2.0 (and EmuTOS, MagiC)
 * calls with already parsed data, regardless of where the bytes came from:
 *
 *   kbdvec   (Kbdvbase() - 4, undocumented but stable since TOS 2.0)
 *            called with d0.b = raw IKBD scancode (bit 7 = release),
 *            a0 = keyboard iorec.
 *   mousevec (Kbdvbase()->mousevec)
 *            called with a0 -> relative mouse packet: header, dx, dy.
 *
 * Neither handler chains to the previous one, so TOS stops buffering keys
 * (no keyboard click / buffer-full beeps) and the GEM mouse cursor stays
 * still while the game runs. Both vectors are restored on exit.
 *
 * This file is part of Atari Quake and is released under the GNU GPL v2 or
 * later, like the rest of the sources.
 */

#include <mint/osbind.h>
#include <mint/ostruct.h>
#include <mint/sysvars.h>

#include "quakedef.h"
#include "in_atari.h"
#include "keys_atari_vec.h"

/* state written from interrupt context */
volatile long          q_mouse_dx __attribute__((aligned(4)));
volatile long          q_mouse_dy __attribute__((aligned(4)));
volatile unsigned char q_mouse_buttons;     /* bit 1 = left, bit 0 = right */

extern unsigned char g_scancodeBuffer[];
extern int g_scancodeBufferHead;

static _KBDVECS* kbdvecs;
static long      old_kbdvec;
static long      old_mousevec;
static qboolean  hooked = false;

/*
 * a.out m68k-atari-mint prefixes C symbols with '_', m68k-atari-mintelf does
 * not; build the asm names from the compiler's own prefix so both work.
 */
#define Q_STR2(x) #x
#define Q_STR(x)  Q_STR2(x)
#define SYM(name) Q_STR(__USER_LABEL_PREFIX__) #name

/*
 * The handlers are called with jsr from the TOS keyboard interrupt and must
 * return with rts, preserving every register they use.
 */
__asm__(
"	.text\n"
"	.even\n"
"	.globl	" SYM(q_kbdvec_handler) "\n"
SYM(q_kbdvec_handler) ":\n"
"	move.l	%d1,-(%sp)\n"
"	move.l	%a1,-(%sp)\n"
"	lea	" SYM(g_scancodeBuffer) ",%a1\n"
"	move.l	" SYM(g_scancodeBufferHead) ",%d1\n"
"	move.b	%d0,(%a1,%d1.l)\n"
"	addq.l	#1,%d1\n"
"	and.l	#255,%d1\n"              /* SCANCODE_BUFFER_SIZE - 1 */
"	move.l	%d1," SYM(g_scancodeBufferHead) "\n"
"	move.l	(%sp)+,%a1\n"
"	move.l	(%sp)+,%d1\n"
"	rts\n"
"\n"
"	.even\n"
"	.globl	" SYM(q_mousevec_handler) "\n"
SYM(q_mousevec_handler) ":\n"
"	move.l	%d0,-(%sp)\n"
"	move.b	(%a0),%d0\n"
"	and.b	#3,%d0\n"
"	move.b	%d0," SYM(q_mouse_buttons) "\n"
"	move.b	1(%a0),%d0\n"
"	ext.w	%d0\n"
"	ext.l	%d0\n"
"	add.l	%d0," SYM(q_mouse_dx) "\n"
"	move.b	2(%a0),%d0\n"
"	ext.w	%d0\n"
"	ext.l	%d0\n"
"	add.l	%d0," SYM(q_mouse_dy) "\n"
"	move.l	(%sp)+,%d0\n"
"	rts\n"
);

extern void q_kbdvec_handler(void);
extern void q_mousevec_handler(void);

static long install_vectors(void)
{
	long* kbdvec = ((long*)kbdvecs) - 1;
	long  sr;

	/* mask interrupts while swapping so no packet sees a half-set state */
	__asm__ volatile ("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "cc");

	old_kbdvec   = *kbdvec;
	old_mousevec = (long)kbdvecs->mousevec;

	q_mouse_dx = 0;
	q_mouse_dy = 0;
	q_mouse_buttons = 0;

	*kbdvec = (long)q_kbdvec_handler;
	kbdvecs->mousevec = (void*)q_mousevec_handler;

	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "cc");
	return 0;
}

static long restore_vectors(void)
{
	long* kbdvec = ((long*)kbdvecs) - 1;
	long  sr;

	__asm__ volatile ("move.w %%sr,%0\n\tor.w #0x0700,%%sr" : "=d"(sr) : : "cc");
	*kbdvec = old_kbdvec;
	kbdvecs->mousevec = (void*)old_mousevec;
	__asm__ volatile ("move.w %0,%%sr" : : "d"(sr) : "cc");
	return 0;
}

static long get_tos_version(void)
{
	OSHEADER* os = *(OSHEADER**)0x4f2;     /* _sysbase */
	os = os->os_beg;
	return os->os_version;
}

qboolean atari_vec_supported(void)
{
	return Supexec(get_tos_version) >= 0x0200;
}

qboolean atari_vec_init(void)
{
	if (hooked)
		return true;

	if (!atari_vec_supported())
		return false;

	kbdvecs = (_KBDVECS*)Kbdvbase();
	if (kbdvecs == NULL)
		return false;

	/* drain whatever is already buffered so it does not leak into the game */
	while (Bconstat(2))
		(void)Bconin(2);

	Supexec(install_vectors);
	hooked = true;
	return true;
}

void atari_vec_shutdown(void)
{
	if (!hooked)
		return;

	Supexec(restore_vectors);
	hooked = false;

	/* drop anything that piled up while the game owned the keyboard */
	while (Bconstat(2))
		(void)Bconin(2);
}

/*
 * Take the accumulated mouse movement. sub.l to memory is a single
 * read-modify-write instruction, so a packet arriving between the read and
 * the subtract is kept for the next frame instead of being lost.
 */
void atari_vec_take_mouse(int* dx, int* dy, int* buttons)
{
	long x, y;

	__asm__ volatile (
		"move.l %2,%0\n\t"
		"sub.l  %0,%2\n\t"
		"move.l %3,%1\n\t"
		"sub.l  %1,%3"
		: "=&d"(x), "=&d"(y), "+m"(q_mouse_dx), "+m"(q_mouse_dy)
		:
		: "cc");

	*dx = (int)x;
	*dy = (int)y;
	*buttons = q_mouse_buttons;
}
