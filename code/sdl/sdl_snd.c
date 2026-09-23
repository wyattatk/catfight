/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include <stdlib.h>
#include <stdio.h>

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#else
#	include <SDL.h>
#endif

#include "../qcommon/q_shared.h"
#include "../client/snd_local.h"
#include "../client/client.h"

qboolean snd_inited = qfalse;

cvar_t *s_sdlBits;
cvar_t *s_sdlSpeed;
cvar_t *s_sdlChannels;
cvar_t *s_sdlDevSamps;
cvar_t *s_sdlMixSamps;

/* The audio callback. All the magic happens here. */
static int dmapos = 0;
static int dmasize = 0;

static SDL_AudioDeviceID sdlPlaybackDevice;

// catfight: the playback device, by name. Out here rather than beside
// s_captureDevice below because playback exists whether or not capture was
// compiled in -- inside that block it builds on this machine and breaks on
// any configuration without VOIP.
static cvar_t *s_device;

#if defined USE_VOIP && SDL_VERSION_ATLEAST( 2, 0, 5 )
#define USE_SDL_AUDIO_CAPTURE

static SDL_AudioDeviceID sdlCaptureDevice;
static cvar_t *s_sdlCapture;
static cvar_t *s_captureDevice;
static float sdlMasterGain = 1.0f;
#endif


/*
===============
SNDDMA_AudioCallback
===============
*/
static void SNDDMA_AudioCallback(void *userdata, Uint8 *stream, int len)
{
	int pos = (dmapos * (dma.samplebits/8));
	if (pos >= dmasize)
		dmapos = pos = 0;

	if (!snd_inited)  /* shouldn't happen, but just in case... */
	{
		memset(stream, '\0', len);
		return;
	}
	else
	{
		int tobufend = dmasize - pos;  /* bytes to buffer's end. */
		int len1 = len;
		int len2 = 0;

		if (len1 > tobufend)
		{
			len1 = tobufend;
			len2 = len - len1;
		}
		memcpy(stream, dma.buffer + pos, len1);
		if (len2 <= 0)
			dmapos += (len1 / (dma.samplebits/8));
		else  /* wraparound? */
		{
			memcpy(stream+len1, dma.buffer, len2);
			dmapos = (len2 / (dma.samplebits/8));
		}
	}

	if (dmapos >= dmasize)
		dmapos = 0;

#ifdef USE_SDL_AUDIO_CAPTURE
	if (sdlMasterGain != 1.0f)
	{
		int i;
		if (dma.isfloat && (dma.samplebits == 32))
		{
			float *ptr = (float *) stream;
			len /= sizeof (*ptr);
			for (i = 0; i < len; i++, ptr++)
			{
				*ptr *= sdlMasterGain;
			}
		}
		else if (dma.samplebits == 16)
		{
			Sint16 *ptr = (Sint16 *) stream;
			len /= sizeof (*ptr);
			for (i = 0; i < len; i++, ptr++)
			{
				*ptr = (Sint16) (((float) *ptr) * sdlMasterGain);
			}
		}
		else if (dma.samplebits == 8)
		{
			Uint8 *ptr = (Uint8 *) stream;
			len /= sizeof (*ptr);
			for (i = 0; i < len; i++, ptr++)
			{
				*ptr = (Uint8) (((float) *ptr) * sdlMasterGain);
			}
		}
	}
#endif
}

static struct
{
	Uint16	enumFormat;
	char		*stringFormat;
} formatToStringTable[ ] =
{
	{ AUDIO_U8,     "AUDIO_U8" },
	{ AUDIO_S8,     "AUDIO_S8" },
	{ AUDIO_U16LSB, "AUDIO_U16LSB" },
	{ AUDIO_S16LSB, "AUDIO_S16LSB" },
	{ AUDIO_U16MSB, "AUDIO_U16MSB" },
	{ AUDIO_S16MSB, "AUDIO_S16MSB" },
	{ AUDIO_F32LSB, "AUDIO_F32LSB" },
	{ AUDIO_F32MSB, "AUDIO_F32MSB" }
};

static int formatToStringTableSize = ARRAY_LEN( formatToStringTable );

/*
===============
SNDDMA_PrintAudiospec
===============
*/
static void SNDDMA_PrintAudiospec(const char *str, const SDL_AudioSpec *spec)
{
	int		i;
	char	*fmt = NULL;

	Com_Printf("%s:\n", str);

	for( i = 0; i < formatToStringTableSize; i++ ) {
		if( spec->format == formatToStringTable[ i ].enumFormat ) {
			fmt = formatToStringTable[ i ].stringFormat;
		}
	}

	if( fmt ) {
		Com_Printf( "  Format:   %s\n", fmt );
	} else {
		Com_Printf( "  Format:   " S_COLOR_RED "UNKNOWN\n");
	}

	Com_Printf( "  Freq:     %d\n", (int) spec->freq );
	Com_Printf( "  Samples:  %d\n", (int) spec->samples );
	Com_Printf( "  Channels: %d\n", (int) spec->channels );
}

/*
===============
SNDDMA_Init
===============
*/
/*
===============
S_ListAudioDevices

What SDL can see, for the audio menu to offer.

Asked live rather than cached at init on purpose: headsets are plugged in and
unplugged while the game is running, and a menu listing what was true at
startup is exactly the menu that cannot fix the problem the player opened it to
fix.
===============
*/
int S_ListAudioDevices( qboolean capture, int index, char *buffer, int bufferSize )
{
	int count;

	if ( buffer && bufferSize > 0 ) {
		buffer[0] = '\0';
	}

	// Enumerating before SDL's audio subsystem is up returns -1, which is not
	// an error here -- it means "none yet".
	count = SDL_GetNumAudioDevices( capture ? SDL_TRUE : SDL_FALSE );
	if ( count < 0 ) {
		return 0;
	}

	if ( buffer && bufferSize > 0 && index >= 0 && index < count ) {
		const char *name = SDL_GetAudioDeviceName( index, capture ? SDL_TRUE : SDL_FALSE );

		if ( name ) {
			Q_strncpyz( buffer, name, bufferSize );
		}
	}
	return count;
}

qboolean SNDDMA_Init(void)
{
	SDL_AudioSpec desired;
	SDL_AudioSpec obtained;
	int tmp;

	if (snd_inited)
		return qtrue;

	if (!s_sdlBits) {
		s_sdlBits = Cvar_Get("s_sdlBits", "16", CVAR_ARCHIVE);
		s_sdlSpeed = Cvar_Get("s_sdlSpeed", "0", CVAR_ARCHIVE);
		s_sdlChannels = Cvar_Get("s_sdlChannels", "2", CVAR_ARCHIVE);
		s_sdlDevSamps = Cvar_Get("s_sdlDevSamps", "0", CVAR_ARCHIVE);
		s_sdlMixSamps = Cvar_Get("s_sdlMixSamps", "0", CVAR_ARCHIVE);
	}

	/*
	catfight: the playback half of what s_captureDevice already does below.

	Empty means "whatever SDL calls the default", which is what was here before
	and is right on most machines. It is not right on the machine with a headset,
	a monitor over HDMI and a virtual cable installed -- and the failure mode is
	the same one that cost an evening on the capture side: the open SUCCEEDS and
	the sound goes somewhere nobody is listening.

	LATCH because the device is opened once during sound init. Changing it takes
	an snd_restart, which the audio menu issues for you.
	*/
	if (!s_device) {
		s_device = Cvar_Get("s_device", "", CVAR_ARCHIVE | CVAR_LATCH);
	}

	Com_Printf( "SDL_Init( SDL_INIT_AUDIO )... " );

	if (SDL_Init(SDL_INIT_AUDIO) != 0)
	{
		Com_Printf( "FAILED (%s)\n", SDL_GetError( ) );
		return qfalse;
	}

	Com_Printf( "OK\n" );

	Com_Printf( "SDL audio driver is \"%s\".\n", SDL_GetCurrentAudioDriver( ) );

	memset(&desired, '\0', sizeof (desired));
	memset(&obtained, '\0', sizeof (obtained));

	tmp = ((int) s_sdlBits->value);
	if ((tmp != 16) && (tmp != 8))
		tmp = 16;

	desired.freq = (int) s_sdlSpeed->value;
	if(!desired.freq) desired.freq = 22050;
	desired.format = ((tmp == 16) ? AUDIO_S16SYS : AUDIO_U8);

	// I dunno if this is the best idea, but I'll give it a try...
	//  should probably check a cvar for this...
	if (s_sdlDevSamps->value)
		desired.samples = s_sdlDevSamps->value;
	else
	{
		// just pick a sane default.
		if (desired.freq <= 11025)
			desired.samples = 256;
		else if (desired.freq <= 22050)
			desired.samples = 512;
		else if (desired.freq <= 44100)
			desired.samples = 1024;
		else
			desired.samples = 2048;  // (*shrug*)
	}

	desired.channels = (int) s_sdlChannels->value;
	desired.callback = SNDDMA_AudioCallback;

	/*
	catfight: named playback device, listed so somebody can see the names.

	Same shape as the capture block below, and for the same reason -- a name
	that no longer matches anything (a headset unplugged since it was chosen)
	falls back to the default rather than failing to start the game with no
	sound and no explanation.
	*/
	{
		const char *want = s_device->string;
		const char *use = NULL;
		int i, count;

		count = SDL_GetNumAudioDevices(SDL_FALSE);
		if (count > 0)
		{
			Com_Printf("Available playback devices:\n");
			for (i = 0; i < count; i++)
			{
				const char *name = SDL_GetAudioDeviceName(i, SDL_FALSE);

				if (!name)
					continue;
				if (want[0] && !Q_stricmp(name, want))
					use = name;
				Com_Printf("  %s%s\n", name, use == name ? "  <- s_device" : "");
			}
		}
		if (want[0] && !use)
			Com_Printf("s_device \"%s\" is not present; using the default\n", want);

		sdlPlaybackDevice = SDL_OpenAudioDevice(use, SDL_FALSE, &desired, &obtained, SDL_AUDIO_ALLOW_ANY_CHANGE);
	}
	if (sdlPlaybackDevice == 0)
	{
		Com_Printf("SDL_OpenAudioDevice() failed: %s\n", SDL_GetError());
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return qfalse;
	}

	SNDDMA_PrintAudiospec("SDL_AudioSpec", &obtained);

	// dma.samples needs to be big, or id's mixer will just refuse to
	//  work at all; we need to keep it significantly bigger than the
	//  amount of SDL callback samples, and just copy a little each time
	//  the callback runs.
	// 32768 is what the OSS driver filled in here on my system. I don't
	//  know if it's a good value overall, but at least we know it's
	//  reasonable...this is why I let the user override.
	tmp = s_sdlMixSamps->value;
	if (!tmp)
		tmp = (obtained.samples * obtained.channels) * 10;

	// samples must be divisible by number of channels
	tmp -= tmp % obtained.channels;

	dmapos = 0;
	dma.samplebits = SDL_AUDIO_BITSIZE(obtained.format);
	dma.isfloat = SDL_AUDIO_ISFLOAT(obtained.format);
	dma.channels = obtained.channels;
	dma.samples = tmp;
	dma.fullsamples = dma.samples / dma.channels;
	dma.submission_chunk = 1;
	dma.speed = obtained.freq;
	dmasize = (dma.samples * (dma.samplebits/8));
	dma.buffer = calloc(1, dmasize);

#ifdef USE_SDL_AUDIO_CAPTURE
	// !!! FIXME: some of these SDL_OpenAudioDevice() values should be cvars.
	s_sdlCapture = Cvar_Get( "s_sdlCapture", "1", CVAR_ARCHIVE | CVAR_LATCH );
	// Empty means "whatever SDL calls the default", which is the behaviour that
	// was here before and is right on most machines. LATCH because the device is
	// opened once during sound init.
	s_captureDevice = Cvar_Get( "s_captureDevice", "", CVAR_ARCHIVE | CVAR_LATCH );
	if (!s_sdlCapture->integer)
	{
		Com_Printf("SDL audio capture support disabled by user ('+set s_sdlCapture 1' to enable)\n");
	}
#if USE_MUMBLE
	else if (cl_useMumble->integer)
	{
		Com_Printf("SDL audio capture support disabled for Mumble support\n");
	}
#endif
	else
	{
		/*
		catfight: this used to be the upstream FIXME "list available devices and
		let cvar specify one, like OpenAL does", and it was worth doing.

		Passing NULL asks SDL for its idea of the default input, which does not
		always agree with the one Windows shows in Sound settings -- and when it
		picks a virtual device (a streaming microphone, an HDMI input) the open
		SUCCEEDS and every sample it delivers is zero. There is no error to
		notice: the game records a perfectly valid silence. That cost an evening
		to find with a microphone that worked fine in every other application,
		so the devices are listed here where somebody can see them, and
		s_captureDevice takes the name of the one to use.
		*/
		SDL_AudioSpec spec;
		const char *want = s_captureDevice->string;
		const char *use = NULL;
		int i, count;

		SDL_zero(spec);
		spec.freq = 48000;
		spec.format = AUDIO_S16SYS;
		spec.channels = 1;
		spec.samples = VOIP_MAX_PACKET_SAMPLES * 4;

		count = SDL_GetNumAudioDevices(SDL_TRUE);
		if (count < 0)
			Com_Printf("SDL cannot enumerate capture devices.\n");
		else if (count == 0)
			Com_Printf("SDL found no capture devices.\n");
		else
		{
			Com_Printf("Available capture devices:\n");
			for (i = 0; i < count; i++)
			{
				const char *name = SDL_GetAudioDeviceName(i, SDL_TRUE);

				if (!name)
					continue;
				if (want[0] && !Q_stricmp(name, want))
					use = name;
				Com_Printf("  %s%s\n", name,
						   (want[0] && !Q_stricmp(name, want)) ? "  <- s_captureDevice" : "");
			}
		}

		if (want[0] && !use)
			Com_Printf(S_COLOR_YELLOW "s_captureDevice \"%s\" is not in that list; "
					   "using the default instead.\n", want);

		sdlCaptureDevice = SDL_OpenAudioDevice(use, SDL_TRUE, &spec, NULL, 0);
		Com_Printf("SDL capture device %s (%s).\n",
				   (sdlCaptureDevice == 0) ? "failed to open" : "opened",
				   use ? use : "system default");
		if (sdlCaptureDevice != 0 && !use)
			Com_Printf("If nothing is recorded, set s_captureDevice to one of the "
					   "names above and restart.\n");
	}

	sdlMasterGain = 1.0f;
#endif

	Com_Printf("Starting SDL audio callback...\n");
	SDL_PauseAudioDevice(sdlPlaybackDevice, 0);  // start callback.
	// don't unpause the capture device; we'll do that in StartCapture.

	Com_Printf("SDL audio initialized.\n");
	snd_inited = qtrue;
	return qtrue;
}

/*
===============
SNDDMA_GetDMAPos
===============
*/
int SNDDMA_GetDMAPos(void)
{
	return dmapos;
}

/*
===============
SNDDMA_Shutdown
===============
*/
void SNDDMA_Shutdown(void)
{
	if (sdlPlaybackDevice != 0)
	{
		Com_Printf("Closing SDL audio playback device...\n");
		SDL_CloseAudioDevice(sdlPlaybackDevice);
		Com_Printf("SDL audio playback device closed.\n");
		sdlPlaybackDevice = 0;
	}

#ifdef USE_SDL_AUDIO_CAPTURE
	if (sdlCaptureDevice)
	{
		Com_Printf("Closing SDL audio capture device...\n");
		SDL_CloseAudioDevice(sdlCaptureDevice);
		Com_Printf("SDL audio capture device closed.\n");
		sdlCaptureDevice = 0;
	}
#endif

	SDL_QuitSubSystem(SDL_INIT_AUDIO);
	free(dma.buffer);
	dma.buffer = NULL;
	dmapos = dmasize = 0;
	snd_inited = qfalse;
	Com_Printf("SDL audio shut down.\n");
}

/*
===============
SNDDMA_Submit

Send sound to device if buffer isn't really the dma buffer
===============
*/
void SNDDMA_Submit(void)
{
	SDL_UnlockAudioDevice(sdlPlaybackDevice);
}

/*
===============
SNDDMA_BeginPainting
===============
*/
void SNDDMA_BeginPainting (void)
{
	SDL_LockAudioDevice(sdlPlaybackDevice);
}


#ifdef USE_VOIP
void SNDDMA_StartCapture(void)
{
#ifdef USE_SDL_AUDIO_CAPTURE
	if (sdlCaptureDevice)
	{
		SDL_ClearQueuedAudio(sdlCaptureDevice);
		SDL_PauseAudioDevice(sdlCaptureDevice, 0);
	}
#endif
}

int SNDDMA_AvailableCaptureSamples(void)
{
#ifdef USE_SDL_AUDIO_CAPTURE
	// divided by 2 to convert from bytes to (mono16) samples.
	return sdlCaptureDevice ? (SDL_GetQueuedAudioSize(sdlCaptureDevice) / 2) : 0;
#else
	return 0;
#endif
}

void SNDDMA_Capture(int samples, byte *data)
{
#ifdef USE_SDL_AUDIO_CAPTURE
	// multiplied by 2 to convert from (mono16) samples to bytes.
	if (sdlCaptureDevice)
	{
		SDL_DequeueAudio(sdlCaptureDevice, data, samples * 2);
	}
	else
#endif
	{
		SDL_memset(data, '\0', samples * 2);
	}
}

void SNDDMA_StopCapture(void)
{
#ifdef USE_SDL_AUDIO_CAPTURE
	if (sdlCaptureDevice)
	{
		SDL_PauseAudioDevice(sdlCaptureDevice, 1);
	}
#endif
}

void SNDDMA_MasterGain( float val )
{
#ifdef USE_SDL_AUDIO_CAPTURE
	sdlMasterGain = val;
#endif
}
#endif

