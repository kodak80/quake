/*
 * snd_atari.c -- sound and dma handling for Atari Falcon 060
 *
 * Copyright (c) 2006 Miro Kropacek; miro.kropacek@gmail.com
 * 
 * This file is part of the Atari Quake project, 3D shooter game by ID Software,
 * for Atari Falcon 060 computers.
 *
 * Atari Quake is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari Quake is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari Quake; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <mint/falcon.h>
#include <mint/osbind.h>
#include <mint/ostruct.h>
#include <mint/cookie.h>

#include "snd_atari_asm.h"
#include "snd_atari_sb.h"

#include "quakedef.h"

dma_t atari_shm;

#define DMA_BUFFER_SIZE	64*1024*2	// 64K for each channel

typedef struct
{
	void* playptr;
	void* recordptr;
	void* reserved1;
	void* reserved2;
} SBUFPTR;

static qboolean isSndInited = false;

#define SND_FALCON  1
#define SND_SB      2
static int sndMode = 0;

#define SB_BUFFER_SIZE	64*1024		// 16384 16-bit stereo frames, power of 2

/*
 * No Falcon DMA sound: try a Sound Blaster on the ISA bus (Raven060).
 * Same 16-bit stereo buffer layout as the Falcon path so the mixer is
 * unchanged; the Timer A interrupt plays it back as 8-bit mono.
 */
static qboolean SNDDMA_InitSB(void)
{
	int freq = 11025;
	int port;

	if( COM_CheckParm( "-nosb" ) )
		return false;

	memset( &atari_shm, 0, sizeof( dma_t ) );
	shm = &atari_shm;

	shm->buffer = (char*)malloc( SB_BUFFER_SIZE + 3 );
	if( shm->buffer == NULL )
	{
		Con_Printf( "Not enough memory for sound buffer\n" );
		return false;
	}
	shm->buffer = (char*)( ( (long)shm->buffer + 3 ) & ~3L );
	memset( shm->buffer, 0, SB_BUFFER_SIZE );

	port = atari_sb_init( shm->buffer, SB_BUFFER_SIZE, &freq );
	if( port == 0 )
	{
		Con_Printf( "Sound system cannot be initialized!\n" );
		return false;
	}

	shm->splitbuffer = 0;
	shm->samplebits = 16;
	shm->channels = 2;
	shm->speed = freq;
	shm->samples = SB_BUFFER_SIZE/2;	// number of mono samples
	shm->submission_chunk = 1;
	shm->samplepos = 0;

	Con_Printf( "Sound Blaster at %x, %d Hz\n", port, freq );
	sndMode = SND_SB;
	isSndInited = true;
	return true;
}

qboolean SNDDMA_Init(void)
{
    long snd_cookie = 0;
    if (Getcookie(C__SND, &snd_cookie) == C_FOUND) {
        // falcon audio is required.
        // most code does xbios access but there is some direct hardware access in snd_atari_asm.s
        if (((snd_cookie & (1 << 1)) == 0) ||
            ((snd_cookie & (1 << 4)) == 0)) {
                snd_cookie = 0;
            }
    }

    if (snd_cookie == 0)
    {
		return SNDDMA_InitSB();
    }

	if( Locksnd() == SNDLOCKED )
	{
		Con_Printf( "Sound system is already in use!\n" );
		return false;
	}
	
	// save sound regs
	sound_atari_init();
	
	memset( &atari_shm, 0, sizeof( dma_t ) );
	
	shm = &atari_shm;
	shm->splitbuffer = 0;
	
	shm->samplebits = 16;
	//shm->speed = 12292;
	shm->speed = 24585;
	//shm->speed = 32880;
	shm->channels = 2;
	
	shm->samples = DMA_BUFFER_SIZE/2;	// number of mono samples
	shm->submission_chunk = 1;
	shm->buffer = (char*)Mxalloc( DMA_BUFFER_SIZE + 1, MX_STRAM );
	if( shm->buffer == NULL )
	{
		Con_Printf("Not enough memory for dma buffer\n");
		return false;
	}
	shm->buffer = (char*)( ( (int)shm->buffer + 1 ) & 0xfffffffe );
	
	memset( shm->buffer, 0, DMA_BUFFER_SIZE );
	
	Sndstatus( SND_RESET );
	Soundcmd( ADDERIN, MATIN );	/* input from connection matrix */
	
	//Devconnect( DMAPLAY, DAC, CLK25M, CLK12K, NO_SHAKE );	/* DMA playback -> DAC */
	Devconnect( DMAPLAY, DAC, CLK25M, CLK25K, NO_SHAKE );	/* DMA playback -> DAC */
	Setmode( MODE_STEREO16 );
	
	Dsptristate( DSP_TRISTATE, DSP_TRISTATE );
	
	Setbuffer( SR_PLAY, shm->buffer, shm->buffer + DMA_BUFFER_SIZE );
	Buffoper( SB_PLA_ENA | SB_PLA_RPT );	/* enable playback in loop mode */
	
	shm->samplepos = 0;
	sndMode = SND_FALCON;
	isSndInited = true;
	return true;
}

int SNDDMA_GetDMAPos(void)
{
	SBUFPTR SDmaBuffers;
	
	if( !isSndInited )
	{
		return 0;
	}

	if( sndMode == SND_SB )
	{
		shm->samplepos = atari_sb_getpos() / 2;
		return shm->samplepos;
	}
	
	if( Buffptr( &SDmaBuffers ) != 0 )
	{
		return 0;
	}

	shm->samplepos = ( (int)SDmaBuffers.playptr - (int)shm->buffer ) / 2;

	return shm->samplepos;

}

void SNDDMA_Shutdown(void)
{
	if( isSndInited && sndMode == SND_SB )
	{
		atari_sb_shutdown();
	}
	else if( isSndInited )
	{
		Buffoper( 0x0000 );	/* disable playback */
		Devconnect( 0x0000, DAC, CLK25M, CLK12K, NO_SHAKE );	/* nothing -> DAC */
		Unlocksnd();
		
		// restore sound regs
		sound_atari_shutdown();
	}
	isSndInited = false;
	sndMode = 0;
}

/*
==============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit(void)
{
}

