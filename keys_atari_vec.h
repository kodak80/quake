#ifndef _KEYS_ATARI_VEC_H_
#define _KEYS_ATARI_VEC_H_

#include "quakedef.h"

/* TOS >= 2.0 kbdvec/mousevec input (Raven060, EmuTOS, clones) */
extern qboolean atari_vec_supported(void);
extern qboolean atari_vec_init(void);
extern void     atari_vec_shutdown(void);
extern void     atari_vec_take_mouse(int* dx, int* dy, int* buttons);

#endif
