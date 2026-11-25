#ifndef _SND_ATARI_SB_H_
#define _SND_ATARI_SB_H_

/*
 * Sound Blaster direct DAC output (no ISA DMA needed), see snd_atari_sb.c
 *
 * buffer: 16-bit signed stereo frames, bytes must be a power of 2
 * freq:   wanted rate in, real Timer A rate out
 * returns the card's port, 0 if no card was found
 */
extern int  atari_sb_init(void* buffer, int bytes, int* freq);
extern int  atari_sb_getpos(void);         /* play position in bytes */
extern void atari_sb_shutdown(void);

#endif
