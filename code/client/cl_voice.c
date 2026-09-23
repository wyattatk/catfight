/*
===========================================================================
catfight

The engine's side of the companion helper.

READ THIS BEFORE EDITING. There is no model, no prompt, no persona and no
inference in this file, and there must never be one. catfight's engine is a
fork of ioquake3 and is GPLv2; the companion's mind -- who she is, what she
remembers, how she is kept in line -- is the part of the project worth
protecting, and it lives in a SEPARATE PROGRAM for exactly that reason. See
cfvoice/README.md. This file talks to it the way it would talk to any other
program: it spawns it and exchanges lines of text.

Everything below is deliberately written in terms of "the helper", never in
terms of a model, because that is the actual contract. If a future change here
needs to know something model-shaped, the answer is a new line in the protocol,
not a dependency.

Structurally this is cl_steam.c with different verbs, on purpose. That file
already solved spawning a child, pumping a non-blocking pipe from the frame
loop, and hex-encoding free text so a space cannot split a message. Diverging
from it would mean two ways to do the same thing and two sets of bugs.

PROTOCOL v1, one message per line, LF-terminated. Full text in cfvoice/README.md.

    ->  hello 1                     us, immediately after spawning
    <-  ready <hex>                 helper: backend up, hex is the model name
    <-  unavailable <hex>           helper: no backend; we fall back to binds

    ->  cmd <seq> <hex>             us, when the player says something
    <-  cmd <seq> none              helper: not an order -- ask her to answer
    <-  cmd <seq> <hex>             helper: one or two cf_cmd lines
    <-  cmderror <seq> <hex>        helper: could not answer

    ->  say <seq> <hex> <hex path>  us, when it turned out not to be an order;
                                    the path is where WE want her voice written
    <-  say <seq> <hex>             helper: her line, no audio
    <-  say <seq> <hex> <hex path>  helper: her line, and audio waiting there
    <-  sayerror <seq> <hex>        helper: could not answer

    ->  stt <seq> <hex path>        us, when the player stops holding the key
    <-  stt <seq> <hex transcript>  helper: what he said
    <-  stterror <seq> <hex>        helper: could not transcribe

    ->  event <hex kind> [<hex detail>]
                                    us, when the match did something that
                                    changes what she is to him
    ->  memory                      us, when he asks what she remembers
    ->  forget <hex which>          us, when he deletes one of them
    <-  memory <hex listing>        helper: the store, for him to read

    ->  quit                        us, on shutdown

EVENTS AND MEMORY CARRY NO SEQUENCE NUMBER, and that is not an omission. The
sequence number exists to supersede an outstanding question; neither of these
is a question and nothing waits on either. Giving them one would put them in
the single-outstanding-request slot, where telling her the round ended would
cancel the order he gave a moment ago.

THE ENGINE STILL DOES NOT KNOW WHAT ANY OF IT MEANS. `kind` is an opaque
string from cgame, this file holds no table of them and no copy of what she
remembers, for the same reason it holds no persona and names no model. What a
match is worth, and what is worth writing down, is the companion's business
and it lives on the far side of the pipe.

AUDIO GOES BY PATH, NOT DOWN THE PIPE. A three-second clip is around 96KB even
after downsampling, and hex-encoding it onto a line would be 192KB through
Sys_ProcessWrite -- which can block when the child is not reading, and blocking
the frame loop on the microphone is exactly the wrong trade in a shooter. The
engine writes a WAV into the homepath and sends where it is; the helper reads
it and deletes it. The pipe stays small and the frame never waits.

ONE UTTERANCE, TWO QUESTIONS. The player does not decide whether he is giving
an order or talking to her, so neither does the engine: everything he says is
classified first, and anything that comes back "none" is handed straight back
as speech. COMMANDS.md's four categories are command, query, information and
chatter, and only the first is an order -- the other three are things she
should ANSWER, and a companion who silently ignores three quarters of what is
said to her is the failure the whole feature exists to avoid.

Chained here rather than inside the helper on purpose. Whether she should be
talking at all is a game question -- mid-firefight, on a cooldown, within an
initiative budget -- and those live on this side of the pipe. The helper stays
two orthogonal verbs, and policy stays where the game state is.

Unrecognised lines are ignored rather than treated as errors, so a newer helper
can say more without breaking an older engine.

WHY THE ENGINE VALIDATES WHAT THE HELPER SENDS. The helper already constrains
the model's decoding with a grammar and re-checks the result in its own code.
This is a third check of the same thing, and it is not redundant: during
development the helper's backend silently DISCARDED that grammar and answered
normally with a completely unconstrained result -- see VOICE.md for how. Layers
one and two live on the swappable side of the pipe. This one does not, and it
is the only thing standing between a confused helper and Cbuf_AddText.
===========================================================================
*/

#include "client.h"



// An utterance is a sentence, not a paragraph. Her replies are longer but
// still bounded; anything past this is the helper malfunctioning.
#define MAX_VOICE_TEXT		1024
/*
A protocol line is the verb, the sequence number, the hex-encoded text, and for
some verbs a hex-encoded PATH as well -- see CL_Voice_Ask.

THE PATH IS WHY THIS IS NOT JUST MAX_VOICE_TEXT * 2 + 64, and leaving it out was
a real bug rather than a tidiness point. `say` carries both a full-length
utterance AND the file the helper should write the audio to, so the worst case
is both encodings plus the framing; sized for the text alone it came up about
five hundred bytes short. Com_sprintf truncates rather than failing, so the
symptom was "Com_sprintf: Output length 2112 too short, require 2113 bytes" and
a companion who stopped answering -- the line still went out, with its last
field cut in half, so what the helper received was a hex path missing its end.

Reported from play as "after she dies any attempt of communication doesn't
work", because a death is what makes her replies long enough to reach the limit.

Fourth instance of this family in the project (HTTP_MAX_BODY, the match ticket,
cl_mm's ticket buffer, now this): A SNUG BUFFER DOES NOT FAIL AS A SIZE ERROR,
it fails as a corrupt message somewhere downstream. Size these against every
field that can be written, not against the biggest one.
*/
#define MAX_VOICE_LINE		( MAX_VOICE_TEXT * 2 + MAX_OSPATH * 2 + 64 )

/*
Her memory, as text, is the one message that is not a sentence: it is
everything she has written down about the player, for him to read and delete.
A few hundred short facts and a stack of matches is comfortably inside this,
and the buffer is sized for the whole thing rather than the listing being
truncated -- a memory control that shows only some of the memory is not one,
and legibility is the entire reason the control exists.

Only the INBOUND accumulator is this big. Nothing the engine sends is anywhere
near it, and sizing the outgoing buffers to match would put 64KB on the stack
of a function that writes forty bytes.
*/
#define MAX_VOICE_MEMORY	( 16 * 1024 )
#define MAX_VOICE_IN		( MAX_VOICE_MEMORY * 2 + 64 )

/*
The situation feed is the other message that is not a sentence: the score, who
is still standing, both their hit rates and the state of the match. It outgrew
MAX_VOICE_TEXT, and quietly truncating it would leave her improvising around
half a number -- which looks exactly like her making things up, because from
her side that is what it is.

Must match CG_VOICE_SITUATION in code/cf_cgame/cg_voice.c.
*/
#define MAX_VOICE_SITUATION		4096
#define MAX_VOICE_STATE_LINE	( MAX_VOICE_SITUATION * 2 + 64 )

/*
The longest line ANY caller can hand CL_Voice_Send.

It exists because there are two line buffers of different sizes and one function
that writes all of them, and sizing that function for either one individually is
how the situation feed came to be truncated on its way out the door. Derived
rather than written as a number, so adding a third kind of line cannot silently
leave this one behind.
*/
#if MAX_VOICE_STATE_LINE > MAX_VOICE_LINE
#define MAX_VOICE_SEND_LINE		MAX_VOICE_STATE_LINE
#else
#define MAX_VOICE_SEND_LINE		MAX_VOICE_LINE
#endif

// How often the stats screen's copy of the memory listing is refreshed behind
// the player's back. Nothing in it changes faster than a match ends.
#define VOICE_MEMORY_REFRESH	1000

// The helper only has to reach a local model for these. A hung helper should
// be reported rather than waited on.
#define VOICE_HELLO_TIMEOUT	180000	// first reply includes loading GBs of weights
#define VOICE_CMD_TIMEOUT	8000
// Speech is a sentence rather than six tokens, so it gets longer -- and unlike
// an order, nothing is waiting on it.
#define VOICE_SAY_TIMEOUT	30000
#define VOICE_STT_TIMEOUT	15000

/*
The microphone.

The engine captures at 48kHz mono 16-bit (sdl_snd.c, shared with VoIP). The
helper is handed 16kHz, which is part of the protocol rather than a property of
anything behind it, so the rate is divided by exactly three on the way in --
averaging each group of three samples, which is a crude low-pass but an
adequate one for speech and costs nothing. Decimating during capture rather
than afterwards also cuts the buffer to a third.

Ten seconds is the cap. A tactical order is one to three; anything past ten is
a stuck key, and a stuck key must not be able to grow a buffer without bound.
*/
#define VOICE_RATE_IN		48000
#define VOICE_RATE_OUT		16000
#define VOICE_DECIMATE		( VOICE_RATE_IN / VOICE_RATE_OUT )
#define VOICE_MAX_SECONDS	10
#define VOICE_MAX_SAMPLES	( VOICE_RATE_OUT * VOICE_MAX_SECONDS )
// Below this there is no utterance, only a key that was tapped by accident.
#define VOICE_MIN_SAMPLES	( VOICE_RATE_OUT / 4 )
// A room with a live microphone in it peaks in the hundreds even when nobody is
// speaking. Below this nothing reached the sound card at all, which is a
// different problem from not being understood and deserves a different message.
#define VOICE_SILENCE_PEAK	64
// Half of full scale. High enough to give the recogniser a strong signal, low
// enough that a loud syllable afterwards does not clip against the ceiling.
#define VOICE_TARGET_PEAK	16000
// A quiet room amplified without bound is just noise, and a recogniser will
// find words in noise. Eight is generous for a quiet microphone and still
// refuses to make something out of nothing.
#define VOICE_MAX_GAIN		8

// Health ticks every frame in a firefight. A second of staleness costs nothing
// because she only reads this when asked to speak; a flood costs everything.
#define VOICE_STATE_INTERVAL	1000

/*
The ways of being out of a match, which is the one part of the situation cgame
cannot describe because cgame does not exist yet. See CL_Voice_BetweenMatches.

NONE means a match is running and cgame owns the feed.
*/
#define VOICE_MENU_NONE			0
#define VOICE_MENU_HOME			1
#define VOICE_MENU_SEARCHING	2
#define VOICE_MENU_FOUND		3

/*
Repeat the out-of-match situation even when it has not changed, for the reason
cgame's CG_VOICE_RESEND exists: a helper restarted under a running game has
been told nothing, and a feed that only speaks on change can never correct
that. Out here it matters more, not less -- the menu is exactly where somebody
restarts her, and she improvises well enough that having been told nothing
looks identical to having been told the truth.
*/
#define VOICE_MENU_RESEND		5000
/*
The gap between things she says off her own back.

This number IS the feature. A companion who remarks on every event is not
company, she is a notification tray, and the failure mode of unprompted speech
is not saying the wrong thing -- it is saying anything at all too often. Start
long. It is far easier to notice that she is too quiet than to un-annoy
somebody who has already decided she talks too much.
*/
#define VOICE_NOTE_COOLDOWN		25000
#define VOICE_CLIP_NAME		"voiceclip.wav"
#define VOICE_SAY_NAME		"voicesay.wav"

/*
Playback. Fed a little ahead of realtime and no further: a six-second line is
around 132,000 samples and the mixer holds 16384, so it goes in across frames.

The lead is computed in samples rather than milliseconds, and that distinction
was paid for. MAX_RAW_SAMPLES is counted AFTER S_RawSamples resamples to the
device rate, so a 22kHz line costs better than two samples of the mixer's
budget for every one submitted on a 48kHz device. A 300ms lead looked modest,
landed at ~13,000 of the 16384 available, and overflowed on the first line long
enough to matter -- which is heard as her being cut off mid-sentence.

So: size the lead against the worst-case device rate, and take half the budget.
*/
#define VOICE_RAW_BUDGET	16384	// MAX_RAW_SAMPLES in snd_local.h
#define VOICE_WORST_DEVICE	48000
#define VOICE_PLAY_CHUNK	4096
// Ten seconds at 22kHz is under half a megabyte; past that the helper is
// misbehaving rather than being talkative.
#define VOICE_PLAY_MAX_BYTES	( 2 * 1024 * 1024 )

typedef enum {
	VOICE_OFF,			// disabled, or no helper binary present
	VOICE_STARTING,		// spawned, hello sent, waiting for a reply
	VOICE_READY,		// backend up, will answer
	VOICE_FAILED		// gone, or told us it cannot help
} voiceState_t;

typedef struct {
	sysProcess_t	*proc;
	voiceState_t	state;
	int				startedAt;

	// Only the newest request is honoured. A round trip is hundreds of
	// milliseconds and a player says the next thing before the last one lands;
	// acting on a superseded order is worse than dropping it.
	int				seq;
	int				pending;		// 0 when nothing is outstanding
	int				askedAt;
	char			pendingWhat[8];	// the verb we are waiting on, for the status readout
	int				pendingTimeout;	// each verb waits a different length

	// Kept so that a reply of "not an order" can be asked again as speech
	// without the player having to say it twice.
	char			utterance[MAX_VOICE_TEXT];

	// Push to talk. The key is held, so the engine knows exactly when speech
	// starts and ends and needs no voice activity detection to find the edges.
	qboolean		recording;
	short			clip[VOICE_MAX_SAMPLES];	// 16kHz mono
	int				clipSamples;
	int				decimSum;					// partial group, across chunks
	int				decimCount;

	// Her voice, fed to the sound system a little ahead of realtime. Paced by
	// the clock rather than by how full the mixer is: the DMA and OpenAL
	// backends track that differently and neither exposes it publicly, so
	// anything that reached into s_rawend would work on one and not the other.
	short			*playPcm;
	int				playSamples;
	int				playSubmitted;
	int				playRate;
	int				playStartedAt;

	// When she last spoke off her own back. cgame reports the events; this is
	// the clock that decides whether she is allowed to act on one.
	int				lastNoteAt;

	/*
	The last thing she said, held for cgame to draw as a subtitle.

	A COUNTER RATHER THAN A TIMESTAMP, and that is the whole design. The engine
	and cgame keep different clocks -- cls.realtime here, server time there --
	so handing over a time would make cgame do arithmetic across two clocks that
	are not the same and are not offset by a constant. Instead cgame notices the
	counter has moved and stamps the line with its OWN clock, which is the one
	it will later compare against to fade it out.

	Held rather than pushed for the same reason CG_VOICE_* exist at all: a push
	from here would have to arrive mid-frame in whatever cgame was doing. cgame
	asks once a frame, when it is ready to draw.
	*/
	char			subtitle[MAX_VOICE_TEXT];
	int				subtitleSeq;

	// Whether the game is actually feeding her. Counted because "is she
	// improvising or does she know?" is otherwise unanswerable from outside,
	// and the difference is the entire point of the state feed.
	int				stateSent;
	int				lastStateAt;
	// The last one, verbatim, for voice_status. What she is told about the
	// match is now the score, who is alive and both their hit rates, and
	// "she said something odd" is unanswerable without being able to see
	// exactly what she was working from.
	char			situation[MAX_VOICE_SITUATION];

	/*
	The last memory listing, and who asked for it.

	Cached because two very different things want it: `voice_memory` prints it
	once, and the stats screen redraws it sixty times a second. Asking the
	helper on every frame would put a pipe round trip inside the render loop
	for a page that changes when a match ends.

	toConsole is per-request rather than a mode: a UI refresh must not print a
	page of text over whatever the player was reading.
	*/
	char			memoryText[MAX_VOICE_MEMORY];
	qboolean		memoryToConsole;
	int				memoryAskedAt;

	/*
	Which out-of-match situation she has been told about, as a VOICE_MENU_*
	value. Zeroed the moment cgame starts describing a match again, so that
	coming back out to the menu always re-sends. See CL_Voice_BetweenMatches.

	A phase rather than a "have I told her yet" flag, because there is more than
	one way to be out of a match and she was being told the wrong one: sitting
	at the home screen and waiting in the matchmaking queue are different
	situations to be talked to in, and the queue is the one with something
	about to happen.
	*/
	int				menuPhase;
	int				menuSentAt;

	// When cl_voiceKey was last refreshed. See CL_Voice_PublishKey.
	int				keyCheckedAt;

	char			model[64];
	char			error[192];

	// Partial line accumulator. The pipe hands us arbitrary chunks; messages
	// are lines. Sized for the largest of them, which is the memory listing.
	char			in[MAX_VOICE_IN];
	int				inLen;
} voiceClient_t;

static voiceClient_t	vc;

static cvar_t	*cl_voice;
static cvar_t	*cl_voiceHelper;
static cvar_t	*cl_voiceState;
// The name of the key that talks to her, for the HUD. See CL_Voice_PublishKey,
// which is defined with the push-to-talk commands it describes and so sits
// below the frame loop that calls it.
static cvar_t	*cl_voiceKey;
static void		CL_Voice_PublishKey( void );
static cvar_t	*cl_voiceChat;
static cvar_t	*cl_voiceKeep;
static cvar_t	*cl_voiceNotice;
static cvar_t	*cl_voiceVolume;

/*
================
The command vocabulary.

COMMANDS.md is the source of truth for this table. If they disagree, this file
is wrong. Kept as data rather than a chain of strcmps so that adding a command
is one line in one place.
================
*/
typedef struct {
	const char	*slot;
	const char	*values[8];		// NULL-terminated; empty means the slot takes none
} voiceSlot_t;

static const voiceSlot_t voiceVocabulary[] = {
	{ "move",	{ "follow", "hold", "push", "fallback", "there", NULL } },
	{ "engage",	{ "free", "return", "hold", NULL } },
	{ "focus",	{ "auto", "this", "player", "bot", NULL } },
	{ "reload",	{ NULL } }
};

// A compound utterance fills at most two slots ("help me" is move follow plus
// focus this). More than that is not a compound, it is the model rambling.
#define MAX_VOICE_COMMANDS	2

static void CL_Voice_SetState( voiceState_t state, const char *text ) {
	vc.state = state;

	switch ( state ) {
		case VOICE_OFF:			Cvar_Set( "cl_voiceState", "off" );			break;
		case VOICE_STARTING:	Cvar_Set( "cl_voiceState", "starting" );	break;
		case VOICE_READY:		Cvar_Set( "cl_voiceState", "ready" );		break;
		default:				Cvar_Set( "cl_voiceState", "failed" );		break;
	}

	if ( text ) {
		Q_strncpyz( vc.error, text, sizeof( vc.error ) );
	} else {
		vc.error[0] = '\0';
	}
}

/*
================
CL_Voice_HexDecode / CL_Voice_HexEncode

The helper hex-encodes any text it sends us and expects the same going out, for
the same reason cl_steam.c does it: this protocol is space-separated, and a
space or a newline in an utterance would otherwise split one message into two.

Deliberately not shared with cl_steam.c. Two twenty-line helpers are cheaper
than a header that couples the two files, and the day one protocol grows a
different encoding the shared version becomes a liability.
================
*/
static int CL_Voice_HexDigit( char c ) {
	if ( c >= '0' && c <= '9' ) {
		return c - '0';
	}
	if ( c >= 'a' && c <= 'f' ) {
		return c - 'a' + 10;
	}
	if ( c >= 'A' && c <= 'F' ) {
		return c - 'A' + 10;
	}
	return -1;
}

static qboolean CL_Voice_HexDecode( const char *hex, char *out, int outSize ) {
	int	len = (int)strlen( hex );
	int	i, o = 0;

	out[0] = '\0';
	if ( len & 1 ) {
		return qfalse;
	}
	for ( i = 0; i + 1 < len; i += 2 ) {
		int hi = CL_Voice_HexDigit( hex[i] );
		int lo = CL_Voice_HexDigit( hex[i + 1] );

		if ( hi < 0 || lo < 0 ) {
			out[0] = '\0';
			return qfalse;
		}
		if ( o >= outSize - 1 ) {
			out[0] = '\0';
			return qfalse;
		}
		out[o++] = (char)( ( hi << 4 ) | lo );
	}
	out[o] = '\0';
	return qtrue;
}

static void CL_Voice_HexEncode( const char *text, char *out, int outSize ) {
	static const char	digits[] = "0123456789abcdef";
	int					i, o = 0;

	for ( i = 0; text[i] && o + 2 < outSize; i++ ) {
		out[o++] = digits[ ( text[i] >> 4 ) & 0x0f ];
		out[o++] = digits[ text[i] & 0x0f ];
	}
	out[o] = '\0';
}

/*
================
CL_Voice_Ask

Sends one request and makes it the outstanding one.

Bumping the sequence number here is what supersedes whatever was in flight:
the older reply still arrives and is dropped on its number. That is also how
the "not an order, ask her to answer" chain stays honest -- the follow-up is a
new request rather than a reply to the old one, so if the player says something
else in the meantime, the speech is abandoned exactly like an order would be.
================
*/
static void CL_Voice_Send( const char *line ) {
	/*
	SIZED FOR THE LONGEST LINE ANY CALLER CAN HAND IT, which is the state line
	and NOT the ask line -- and getting that wrong is what survived the first
	attempt at this bug.

	Callers build into two different buffers: MAX_VOICE_LINE for a verb with
	hex-encoded text, and MAX_VOICE_STATE_LINE -- nearly twice as long -- for
	the situation feed. This function is the single point every one of them
	funnels through, so it has to fit the larger, and it was sized for the
	smaller. A situation over about 1285 characters hex-encodes past 2634 and
	was cut off here, after every caller had already been careful.

	That is why the first fix did not hold: it widened the ASK path and added a
	guard to the ASK caller, when the overflow was on the STATE path. A choke
	point is the right place to both size and check, because it is the only
	place that sees every case.
	*/
	char	buf[MAX_VOICE_SEND_LINE + 2];
	int		len;

	if ( !vc.proc ) {
		return;
	}

	/*
	Refuse rather than truncate. Com_sprintf would cut the line and warn to a
	console nobody is reading, and the half-line still reaches the helper -- a
	hex string with an odd tail, which decodes to something. A message that
	fails later and somewhere else is the worst outcome available, and it
	presented as her going permanently silent after a death.
	*/
	if ( (int)strlen( line ) + 2 > (int)sizeof( buf ) ) {
		Com_Printf( "voice: refusing to send %d bytes, the line buffer is %d\n",
		            (int)strlen( line ) + 2, (int)sizeof( buf ) );
		return;
	}

	len = Com_sprintf( buf, sizeof( buf ), "%s\n", line );
	if ( Sys_ProcessWrite( vc.proc, buf, len ) < 0 ) {
		CL_Voice_SetState( VOICE_FAILED, "the helper stopped listening" );
	}
}

static void CL_Voice_Ask( const char *verb, const char *text, const char *extra,
						  int timeout ) {
	char	hex[MAX_VOICE_TEXT * 2 + 1];
	char	extraHex[MAX_OSPATH * 2 + 1];
	char	line[MAX_VOICE_LINE];

	vc.pending = ++vc.seq;
	vc.pendingTimeout = timeout;
	vc.askedAt = cls.realtime;
	Q_strncpyz( vc.pendingWhat, verb, sizeof( vc.pendingWhat ) );

	CL_Voice_HexEncode( text, hex, sizeof( hex ) );
	if ( extra ) {
		CL_Voice_HexEncode( extra, extraHex, sizeof( extraHex ) );
		Com_sprintf( line, sizeof( line ), "%s %d %s %s", verb, vc.pending, hex, extraHex );
	} else {
		Com_sprintf( line, sizeof( line ), "%s %d %s", verb, vc.pending, hex );
	}

	/*
	REFUSE A TRUNCATED LINE RATHER THAN SENDING ONE.

	Com_sprintf truncates and warns; the warning goes to the console where
	nobody is looking, and the half-line goes to the helper, where it is a hex
	string with an odd tail and a path missing its end. That decodes to
	something, which is the worst outcome available -- a message that fails
	later and somewhere else.

	The buffer above is now sized for every field, so this should be
	unreachable. It is here because the previous version was also believed to be
	big enough, and the failure was invisible for as long as it was.
	*/
	if ( (int)strlen( line ) >= (int)sizeof( line ) - 1 ) {
		Com_Printf( "voice: refusing to send a truncated %s (%d bytes)\n",
		            verb, (int)strlen( line ) );
		vc.pending = 0;
		return;
	}

	CL_Voice_Send( line );
}

// Where the helper is told to put her synthesised voice. The ENGINE picks it,
// so the helper never has to know anything about where this game keeps files.
static const char *CL_Voice_SayPath( void ) {
	return FS_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), NULL,
						   VOICE_SAY_NAME );
}

/*
================
CL_Voice_Utterance

Everything the player says enters here, whether he typed it or spoke it.

One path on purpose. Speech-to-text is a different way to produce the words,
not a different thing to do with them -- so a bug in how an utterance is
handled can only ever exist once, and the typed path stays a usable stand-in
for the spoken one when a microphone is not to hand.
================
*/
static void CL_Voice_Utterance( const char *text ) {
	// Kept because a reply of "not an order" turns straight around into a
	// speech request, and making the player repeat himself to get an answer
	// would be the most irritating possible way to lose the distinction.
	Q_strncpyz( vc.utterance, text, sizeof( vc.utterance ) );
	CL_Voice_Ask( "cmd", text, NULL, VOICE_CMD_TIMEOUT );
}

/*
================
CL_Voice_WriteClip

Writes what was captured as a 16kHz mono WAV and returns where it landed.

Written by hand rather than through any existing sound code because nothing in
the engine writes audio -- it only reads it. Bytes are emitted little-endian
explicitly: a WAV is little-endian regardless of the host, and the capture
buffer is in host order.
================
*/
static void CL_Voice_PutLE16( byte **p, int v ) {
	( *p )[0] = v & 0xff;
	( *p )[1] = ( v >> 8 ) & 0xff;
	*p += 2;
}

static void CL_Voice_PutLE32( byte **p, int v ) {
	( *p )[0] = v & 0xff;
	( *p )[1] = ( v >> 8 ) & 0xff;
	( *p )[2] = ( v >> 16 ) & 0xff;
	( *p )[3] = ( v >> 24 ) & 0xff;
	*p += 4;
}

static const char *CL_Voice_WriteClip( void ) {
	int		dataBytes = vc.clipSamples * 2;
	int		total = 44 + dataBytes;
	byte	*wav = Z_Malloc( total );
	byte	*p = wav;
	int		i;

	Com_Memcpy( p, "RIFF", 4 );					p += 4;
	CL_Voice_PutLE32( &p, 36 + dataBytes );
	Com_Memcpy( p, "WAVEfmt ", 8 );				p += 8;
	CL_Voice_PutLE32( &p, 16 );					// fmt chunk size
	CL_Voice_PutLE16( &p, 1 );					// PCM
	CL_Voice_PutLE16( &p, 1 );					// mono
	CL_Voice_PutLE32( &p, VOICE_RATE_OUT );
	CL_Voice_PutLE32( &p, VOICE_RATE_OUT * 2 );	// bytes per second
	CL_Voice_PutLE16( &p, 2 );					// block align
	CL_Voice_PutLE16( &p, 16 );					// bits per sample
	Com_Memcpy( p, "data", 4 );					p += 4;
	CL_Voice_PutLE32( &p, dataBytes );

	for ( i = 0; i < vc.clipSamples; i++ ) {
		CL_Voice_PutLE16( &p, vc.clip[i] );
	}

	FS_WriteFile( VOICE_CLIP_NAME, wav, total );
	Z_Free( wav );

	// fs_homedatapath, not fs_homepath: this fork splits the home directory and
	// FS_FOpenFileWrite puts files under the data one. Passing NULL for the
	// game lets FS_BuildOSPath fill in fs_gamedir, exactly as files.c does.
	return FS_BuildOSPath( Cvar_VariableString( "fs_homedatapath" ), NULL,
						   VOICE_CLIP_NAME );
}

/*
================
CL_Voice_PumpCapture

Drains the microphone into the clip, decimating 48kHz to 16kHz as it goes.

Keeps draining after the buffer is full rather than stopping, because the
samples are queued inside SDL and abandoning them there would leave the next
utterance beginning with the tail of this one.
================
*/
static void CL_Voice_PumpCapture( void ) {
	short	buf[1024];
	int		avail, got, i;

	while ( ( avail = S_AvailableCaptureSamples() ) > 0 ) {
		got = avail;
		if ( got > (int)ARRAY_LEN( buf ) ) {
			got = ARRAY_LEN( buf );
		}
		S_Capture( got, (byte *)buf );

		for ( i = 0; i < got; i++ ) {
			vc.decimSum += buf[i];
			if ( ++vc.decimCount < VOICE_DECIMATE ) {
				continue;
			}
			if ( vc.clipSamples < VOICE_MAX_SAMPLES ) {
				vc.clip[vc.clipSamples++] = (short)( vc.decimSum / VOICE_DECIMATE );
			}
			vc.decimSum = 0;
			vc.decimCount = 0;
		}
	}
}

/*
================
CL_Voice_FinishRecording

Stops capture and hands the clip to the helper.
================
*/
static void CL_Voice_FinishRecording( void ) {
	const char	*path;
	char		hex[MAX_VOICE_TEXT * 2 + 1];
	char		line[MAX_VOICE_LINE];

	if ( !vc.recording ) {
		return;
	}
	CL_Voice_PumpCapture();
	S_StopCapture();
	vc.recording = qfalse;

	// A tap of the key is not an utterance. Transcribing a fifth of a second of
	// room tone is how a companion ends up acting on something nobody said.
	if ( vc.clipSamples < VOICE_MIN_SAMPLES ) {
		Com_DPrintf( "voice: too short, ignoring\n" );
		return;
	}

	/*
	How loud was it, actually?

	Worth measuring rather than assuming, because a microphone that records
	NOTHING looks exactly like a microphone that recorded speech the model
	could not make out: samples arrive, the clip is the right length, the WAV
	is well formed, and the only symptom is "didn't catch that" forever.

	The engine opens whatever Windows calls the default recording device --
	sdl_snd.c passes NULL and carries upstream's own FIXME about not being able
	to choose -- so a machine with a virtual capture device set as default
	produces a perfectly valid recording of silence. Telling the player that is
	the difference between a five-minute fix and an evening.
	*/
	{
		int	i, peak = 0;

		for ( i = 0; i < vc.clipSamples; i++ ) {
			int mag = vc.clip[i] < 0 ? -vc.clip[i] : vc.clip[i];

			if ( mag > peak ) {
				peak = mag;
			}
		}
		Com_DPrintf( "voice: %.1fs captured, peak %i of 32767\n",
					 (float)vc.clipSamples / VOICE_RATE_OUT, peak );

		if ( peak < VOICE_SILENCE_PEAK ) {
			Com_Printf( "voice: your microphone recorded silence (peak %i). "
						"catfight records from the Windows DEFAULT input device "
						"-- check that it is the microphone you actually use. "
						"s_captureDevice picks a different one.\n", peak );
			return;
		}

		/*
		Bring the level up before handing it over.

		Microphones arrive at wildly different levels and a typical one here
		peaks around 2000 of 32767 -- about a tenth of full scale. Speech
		recognition degrades on a quiet signal in exactly the way it looked like
		a bad model: "move to my cursor" came back as "love to my cursor", "move
		to my curse", "that's my cursor". Same words, same speaker, a model that
		simply had not much signal to work with.

		Normalising is a couple of lines and costs nothing, where telling every
		player to go and find Windows' microphone boost slider costs them the
		first ten minutes of the game. The gain is capped so that a near-silent
		room is not amplified into a roar of noise for the recogniser to
		hallucinate words out of.
		*/
		if ( peak > 0 && peak < VOICE_TARGET_PEAK ) {
			int	gain = ( VOICE_TARGET_PEAK << 8 ) / peak;	// 8.8 fixed point

			if ( gain > ( VOICE_MAX_GAIN << 8 ) ) {
				gain = VOICE_MAX_GAIN << 8;
			}
			for ( i = 0; i < vc.clipSamples; i++ ) {
				int v = ( vc.clip[i] * gain ) >> 8;

				if ( v > 32767 ) {
					v = 32767;
				} else if ( v < -32768 ) {
					v = -32768;
				}
				vc.clip[i] = (short)v;
			}
			Com_DPrintf( "voice: normalised by %.1fx\n", gain / 256.0f );
		}
	}
	if ( !CL_Voice_Available() ) {
		Com_Printf( "voice: %s\n", vc.error[0] ? vc.error : "the helper is not ready" );
		return;
	}

	path = CL_Voice_WriteClip();

	/*
	Optionally keep a copy.

	Speech recognition is the one part of this pipeline that cannot be tuned
	against synthetic input. Text-to-speech clips transcribe perfectly at any
	level, so they prove nothing: what actually breaks it is a real room, a real
	microphone and the way somebody really talks mid-round. Without keeping the
	failures there is nothing to test a different model against except the
	memory of it going wrong.

	Off by default -- it is a recording of the player's microphone, and keeping
	those without being asked is not something to do quietly.
	*/
	if ( cl_voiceKeep->integer ) {
		static int	kept;
		char		name[MAX_QPATH];
		byte		*copy;
		int			len;

		Com_sprintf( name, sizeof( name ), "voiceclips/clip%04i.wav", kept++ );
		len = FS_ReadFile( VOICE_CLIP_NAME, (void **)&copy );
		if ( len > 0 && copy ) {
			FS_WriteFile( name, copy, len );
			FS_FreeFile( copy );
			Com_DPrintf( "voice: kept %s\n", name );
		}
	}

	vc.pending = ++vc.seq;
	vc.pendingTimeout = VOICE_STT_TIMEOUT;
	vc.askedAt = cls.realtime;
	Q_strncpyz( vc.pendingWhat, "stt", sizeof( vc.pendingWhat ) );

	CL_Voice_HexEncode( path, hex, sizeof( hex ) );
	Com_sprintf( line, sizeof( line ), "stt %d %s", vc.pending, hex );
	CL_Voice_Send( line );
}

/*
================
CL_Voice_ValidCommand

One line, against the vocabulary. See the header for why this exists when the
helper has already checked twice.
================
*/
static qboolean CL_Voice_ValidCommand( const char *line ) {
	char		copy[128];
	char		*slot, *value;
	int			i, v;

	Q_strncpyz( copy, line, sizeof( copy ) );

	if ( Q_strncmp( copy, "cf_cmd ", 7 ) ) {
		return qfalse;
	}
	slot = copy + 7;
	while ( *slot == ' ' ) {
		slot++;
	}

	value = strchr( slot, ' ' );
	if ( value ) {
		*value++ = '\0';
		while ( *value == ' ' ) {
			value++;
		}
		if ( !*value ) {
			value = NULL;
		}
	}

	// A value containing anything else is a second command smuggled onto one
	// line. Refuse rather than pass the first token through.
	if ( value && strchr( value, ' ' ) ) {
		return qfalse;
	}

	for ( i = 0; i < ARRAY_LEN( voiceVocabulary ); i++ ) {
		if ( strcmp( slot, voiceVocabulary[i].slot ) ) {
			continue;
		}
		if ( !voiceVocabulary[i].values[0] ) {
			return (qboolean)( value == NULL );	// slot takes no value
		}
		if ( !value ) {
			return qfalse;
		}
		for ( v = 0; voiceVocabulary[i].values[v]; v++ ) {
			if ( !strcmp( value, voiceVocabulary[i].values[v] ) ) {
				return qtrue;
			}
		}
		return qfalse;
	}
	return qfalse;
}

/*
================
CL_Voice_Execute

Runs what the helper sent, after checking every line of it.

All or nothing. A compound that half-validates is a companion doing one of the
two things the player asked for, and he cannot tell which half went missing --
which is exactly the failure COMMANDS.md says must not happen, because it is
indistinguishable from her being stuck.
================
*/
static void CL_Voice_Execute( char *text ) {
	char	*lines[MAX_VOICE_COMMANDS];
	int		count = 0;
	char	*p = text;
	int		i;

	while ( *p ) {
		char *nl = strchr( p, '\n' );

		if ( nl ) {
			*nl = '\0';
		}
		if ( *p ) {
			// Checked here rather than after the loop. The first version tested
			// leftover input with *p once the loop had finished, which is wrong
			// because p still points at the last line it consumed -- so every
			// reply, including a single well-formed command, was refused as
			// "too many commands".
			if ( count >= MAX_VOICE_COMMANDS ) {
				Com_Printf( "voice: refusing a reply with too many commands\n" );
				return;
			}
			lines[count++] = p;
		}
		if ( !nl ) {
			break;
		}
		p = nl + 1;
	}

	if ( !count ) {
		Com_Printf( "voice: refusing a reply with no commands\n" );
		return;
	}

	for ( i = 0; i < count; i++ ) {
		if ( !CL_Voice_ValidCommand( lines[i] ) ) {
			Com_Printf( "voice: refusing \"%s\"\n", lines[i] );
			return;
		}
	}

	// One slot cannot be set twice in one utterance. "hold here and push" is a
	// player changing his mind mid-sentence, and guessing which half he meant
	// is worse than telling him she did not follow. The helper checks this too;
	// this is the copy on the side of the pipe that cannot be swapped out.
	if ( count == 2 ) {
		const char	*a = lines[0] + 7;		// past "cf_cmd "
		const char	*b = lines[1] + 7;
		int			alen = (int)( strcspn( a, " " ) );

		if ( (int)strcspn( b, " " ) == alen && !Q_strncmp( a, b, alen ) ) {
			Com_Printf( "voice: refusing two orders for the same thing\n" );
			return;
		}
	}

	for ( i = 0; i < count; i++ ) {
		Com_DPrintf( "voice: %s\n", lines[i] );
		Cbuf_AddText( va( "%s\n", lines[i] ) );
	}
}

/*
================
Her voice.

The helper synthesises into a WAV at a path WE chose and told it, so the engine
keeps ownership of the filesystem and the helper only has to write bytes. Read
back with Sys_FOpen rather than through the filesystem: it is a scratch buffer
written a moment ago, not an asset, and the search path has nothing to say
about it. Deleted once read.
================
*/
static int CL_Voice_LE16( const byte *p ) {
	return p[0] | ( p[1] << 8 );
}

static int CL_Voice_LE32( const byte *p ) {
	return p[0] | ( p[1] << 8 ) | ( p[2] << 16 ) | ( p[3] << 24 );
}

static void CL_Voice_StopPlayback( void ) {
	if ( vc.playPcm ) {
		Z_Free( vc.playPcm );
		vc.playPcm = NULL;
	}
	vc.playSamples = 0;
	vc.playSubmitted = 0;
}

static void CL_Voice_PlayClip( const char *ospath ) {
	FILE	*f;
	byte	*buf;
	int		len, pos;
	int		rate = 0, channels = 0, bits = 0;
	const byte *pcm = NULL;
	int		pcmBytes = 0;

	CL_Voice_StopPlayback();

	f = Sys_FOpen( ospath, "rb" );
	if ( !f ) {
		Com_DPrintf( "voice: no audio at %s\n", ospath );
		return;
	}
	fseek( f, 0, SEEK_END );
	len = (int)ftell( f );
	fseek( f, 0, SEEK_SET );

	if ( len < 44 || len > VOICE_PLAY_MAX_BYTES ) {
		fclose( f );
		remove( ospath );
		Com_Printf( "voice: audio is %i bytes, refusing it\n", len );
		return;
	}

	buf = Z_Malloc( len );
	if ( (int)fread( buf, 1, len, f ) != len ) {
		fclose( f );
		remove( ospath );
		Z_Free( buf );
		Com_Printf( "voice: could not read her audio\n" );
		return;
	}
	fclose( f );
	remove( ospath );

	if ( Q_strncmp( (char *)buf, "RIFF", 4 ) || Q_strncmp( (char *)buf + 8, "WAVE", 4 ) ) {
		Z_Free( buf );
		Com_Printf( "voice: that is not a WAV\n" );
		return;
	}

	// Walk the chunks rather than assuming a 44-byte header: a synthesiser is
	// entitled to emit LIST or fact chunks and several do.
	for ( pos = 12; pos + 8 <= len; ) {
		const byte	*id = buf + pos;
		int			size = CL_Voice_LE32( buf + pos + 4 );

		if ( size < 0 || pos + 8 + size > len ) {
			break;
		}
		if ( !Q_strncmp( (char *)id, "fmt ", 4 ) && size >= 16 ) {
			channels = CL_Voice_LE16( buf + pos + 8 + 2 );
			rate     = CL_Voice_LE32( buf + pos + 8 + 4 );
			bits     = CL_Voice_LE16( buf + pos + 8 + 14 );
		} else if ( !Q_strncmp( (char *)id, "data", 4 ) ) {
			pcm = buf + pos + 8;
			pcmBytes = size;
		}
		pos += 8 + size + ( size & 1 );		// chunks are word aligned
	}

	if ( !pcm || bits != 16 || channels != 1 || rate < 8000 || rate > 48000 ) {
		Z_Free( buf );
		Com_Printf( "voice: expected 16-bit mono audio, got %i-bit %i channel at %i Hz\n",
					bits, channels, rate );
		return;
	}

	vc.playSamples = pcmBytes / 2;
	vc.playPcm = Z_Malloc( pcmBytes );
	Com_Memcpy( vc.playPcm, pcm, pcmBytes );
	vc.playRate = rate;
	vc.playSubmitted = 0;
	vc.playStartedAt = cls.realtime;
	Z_Free( buf );

	Com_DPrintf( "voice: %.1fs of audio at %i Hz\n",
				 (float)vc.playSamples / rate, rate );
}

/*
================
CL_Voice_PumpPlayback

Hands the sound system the next slice, keeping a fixed lead over realtime.

Submitting the whole line at once would overrun MAX_RAW_SAMPLES and be heard as
a click and then silence, so the clock decides how much has been earned.
================
*/
static void CL_Voice_PumpPlayback( void ) {
	int	elapsed, want, n, lead;

	if ( !vc.playPcm ) {
		return;
	}

	// Half the mixer's budget, expressed in SOURCE samples for this clip's
	// rate. See the note above VOICE_RAW_BUDGET.
	lead = ( VOICE_RAW_BUDGET / 2 ) * vc.playRate / VOICE_WORST_DEVICE;

	elapsed = cls.realtime - vc.playStartedAt;
	want = (int)( (double)elapsed * vc.playRate / 1000.0 ) + lead;
	if ( want > vc.playSamples ) {
		want = vc.playSamples;
	}

	n = want - vc.playSubmitted;
	if ( n > VOICE_PLAY_CHUNK ) {
		n = VOICE_PLAY_CHUNK;
	}
	if ( n > 0 ) {
		S_RawSamples( RAW_STREAM_COMPANION, n, vc.playRate, 2, 1,
					  (byte *)( vc.playPcm + vc.playSubmitted ),
					  cl_voiceVolume->value, -1 );
		vc.playSubmitted += n;
	}

	if ( vc.playSubmitted >= vc.playSamples ) {
		// The buffer is handed over, not yet heard. Freeing it is safe -- the
		// mixer copied what it was given.
		CL_Voice_StopPlayback();
	}
}

/*
================
CL_Voice_Message

One complete line from the helper.
================
*/
static void CL_Voice_Message( char *line ) {
	char	*verb, *rest;

	// Split off the first token by hand rather than using Cmd_TokenizeString:
	// this runs inside the frame loop and clobbering the command tokenizer
	// under whatever is mid-parse is not worth the convenience.
	verb = line;
	rest = strchr( line, ' ' );
	if ( rest ) {
		*rest++ = '\0';
		while ( *rest == ' ' ) {
			rest++;
		}
	} else {
		rest = line + strlen( line );
	}

	if ( !strcmp( verb, "ready" ) ) {
		CL_Voice_HexDecode( rest, vc.model, sizeof( vc.model ) );
		CL_Voice_SetState( VOICE_READY, NULL );
		Com_DPrintf( "voice: helper ready (%s)\n", vc.model );
		// He is back. Sent here rather than at spawn because the helper is
		// only listening once it has answered, and it is the first thing
		// worth telling her: everything she remembers is dated against these.
		CL_Voice_Event( "session-start", NULL );
		return;
	}

	if ( !strcmp( verb, "unavailable" ) ) {
		// Not a failure of ours and not worth a warning colour. The keybinds
		// emit the identical cf_cmd, so the game is playable.
		char	why[sizeof( vc.error )];

		CL_Voice_HexDecode( rest, why, sizeof( why ) );
		CL_Voice_SetState( VOICE_FAILED, why[0] ? why : "the companion helper is not available" );

		/*
		Printed, not Com_DPrintf'd, and that is a change of mind.

		It was at developer level because the reasons were all developer
		problems -- no model file, no binary, a path wrong on somebody's build.
		The helper now also refuses when the machine is simply too slow to run
		her, and that reason is FOR THE PLAYER: it is the difference between a
		companion who is quietly absent and a sentence explaining what would
		fix it. Nobody is going to set developer 1 to find that out.
		*/
		Com_Printf( S_COLOR_YELLOW "companion: %s\n", vc.error );
		return;
	}

	/*
	Everything she remembers, on its way to the console.

	No sequence number, and it deliberately does not touch vc.pending: reading
	his own memory file is not a question she has to think about, and putting
	it in the single-outstanding-request slot would mean typing voice_memory
	cancelled the order he gave a moment ago.
	*/
	if ( !strcmp( verb, "memory" ) ) {
		char	*at, *next;

		if ( !CL_Voice_HexDecode( rest, vc.memoryText, sizeof( vc.memoryText ) ) ) {
			Com_Printf( "voice: unreadable memory listing\n" );
			return;
		}

		// Kept whoever asked; printed only for whoever typed.
		if ( !vc.memoryToConsole ) {
			return;
		}
		vc.memoryToConsole = qfalse;

		{
		static char	text[MAX_VOICE_MEMORY];

		Q_strncpyz( text, vc.memoryText, sizeof( text ) );
		// A line at a time. Com_Printf has a buffer of its own and drops
		// everything past it without saying so, which for this message would
		// mean showing him part of what she remembers and calling it all of
		// it -- the exact failure the control exists to prevent.
		for ( at = text; *at; at = next ) {
			next = strchr( at, '\n' );
			if ( next ) {
				*next++ = '\0';
			} else {
				next = at + strlen( at );
			}
			Com_Printf( "%s\n", at );
		}
		}
		return;
	}

	/*
	How a line reads to her, asked directly by voice_tone.

	Kept out of the pending slot for the same reason `memory` is: it is a
	tuning question typed at a console, and it must not be able to cancel an
	order given a moment earlier.
	*/
	if ( !strcmp( verb, "tone" ) || !strcmp( verb, "toneerror" ) ) {
		char	*payload = strchr( rest, ' ' );
		char	what[MAX_VOICE_TEXT];

		if ( payload ) {
			payload++;
		}
		if ( !payload || !CL_Voice_HexDecode( payload, what, sizeof( what ) ) ) {
			Com_Printf( "voice: unreadable tone reply\n" );
			return;
		}
		Com_Printf( "tone: %s\n", what );
		return;
	}

	if ( !strcmp( verb, "cmd" ) || !strcmp( verb, "cmderror" )
		 || !strcmp( verb, "say" ) || !strcmp( verb, "sayerror" )
		 || !strcmp( verb, "stt" ) || !strcmp( verb, "stterror" ) ) {
		char	*payload = strchr( rest, ' ' );
		int		seq = atoi( rest );

		if ( payload ) {
			*payload++ = '\0';
		}

		// Stale. The player has said something since, and the newer answer is
		// the one that reflects what he actually wants.
		if ( seq != vc.pending ) {
			Com_DPrintf( "voice: dropping stale reply %d (waiting on %d)\n", seq, vc.pending );
			return;
		}
		vc.pending = 0;

		if ( !strcmp( verb, "cmderror" ) || !strcmp( verb, "sayerror" )
			 || !strcmp( verb, "stterror" ) ) {
			char	why[sizeof( vc.error )];

			CL_Voice_HexDecode( payload ? payload : "", why, sizeof( why ) );
			Com_Printf( "voice: %s\n", why[0] ? why : "the helper could not answer" );
			return;
		}

		if ( !strcmp( verb, "stt" ) ) {
			char	text[MAX_VOICE_TEXT];

			if ( !payload || !CL_Voice_HexDecode( payload, text, sizeof( text ) )
				 || !text[0] ) {
				Com_Printf( "voice: didn't catch that\n" );
				return;
			}
			// Show him what was heard before acting on it. COMMANDS.md is
			// explicit that the player's first question is always "did she hear
			// me?", and a visible wrong transcript reads as a misunderstanding
			// he can correct -- an invisible one reads as her being broken.
			Com_Printf( S_COLOR_YELLOW "you: %s\n", text );
			CL_Voice_Utterance( text );
			return;
		}

		if ( !strcmp( verb, "say" ) ) {
			char	text[MAX_VOICE_TEXT];
			char	*audioHex = payload ? strchr( payload, ' ' ) : NULL;

			if ( audioHex ) {
				*audioHex++ = '\0';
			}
			if ( !payload || !CL_Voice_HexDecode( payload, text, sizeof( text ) ) ) {
				Com_Printf( "voice: unreadable reply\n" );
				return;
			}
			// A second field means she has a voice for this line. Its absence
			// is not an error -- speech synthesis is optional in exactly the
			// way speech recognition is, and she still says it in text.
			if ( audioHex ) {
				char	apath[MAX_OSPATH];

				if ( CL_Voice_HexDecode( audioHex, apath, sizeof( apath ) ) ) {
					CL_Voice_PlayClip( apath );
				}
			}
			// Printed with no name in front of it. THE ENGINE DOES NOT KNOW WHO
			// SHE IS -- her name is persona data and it lives in the helper,
			// for the same reason nothing here names a model. Presentation is
			// cgame's job; this is the mechanism underneath it.
			Com_Printf( S_COLOR_CYAN "%s\n", text );

			/*
			And here is cgame doing that job. Handed over verbatim -- no name,
			no colour, no wrapping -- because every one of those is a
			presentation decision and none of them belong on this side of the
			boundary.

			Stored even when there is no audio. A line she had no voice for is
			exactly the line a subtitle is most needed for, and the branch above
			already treats missing speech as normal rather than as an error.
			*/
			Q_strncpyz( vc.subtitle, text, sizeof( vc.subtitle ) );
			vc.subtitleSeq++;
			return;
		}

		// "none" is the COMMON case, not the error case: most of what a player
		// says is a question, a callout or chatter. None of it is an order, and
		// all of it is something she should answer -- so it goes straight back
		// as speech rather than being dropped on the floor.
		if ( payload && !strcmp( payload, "none" ) ) {
			if ( cl_voiceChat->integer && vc.utterance[0] ) {
				CL_Voice_Ask( "say", vc.utterance, CL_Voice_SayPath(), VOICE_SAY_TIMEOUT );
			} else {
				Com_DPrintf( "voice: not an order\n" );
			}
			return;
		}

		{
			char	text[MAX_VOICE_TEXT];

			if ( !payload || !CL_Voice_HexDecode( payload, text, sizeof( text ) ) ) {
				Com_Printf( "voice: unreadable reply\n" );
				return;
			}
			CL_Voice_Execute( text );
		}
		return;
	}

	// Anything else is from a newer helper than this engine. Ignoring it is
	// what makes the protocol extensible in the direction that matters.
}

/*
================
CL_Voice_Pump

Drains whatever the pipe has and dispatches complete lines.
================
*/
static void CL_Voice_Pump( void ) {
	char	buf[1024];
	int		got, i;

	while ( ( got = Sys_ProcessRead( vc.proc, buf, sizeof( buf ) ) ) > 0 ) {
		for ( i = 0; i < got; i++ ) {
			char c = buf[i];

			if ( c == '\r' ) {
				continue;
			}
			if ( c == '\n' ) {
				vc.in[vc.inLen] = '\0';
				if ( vc.inLen > 0 ) {
					CL_Voice_Message( vc.in );
				}
				vc.inLen = 0;
				continue;
			}
			if ( vc.inLen < (int)sizeof( vc.in ) - 1 ) {
				vc.in[vc.inLen++] = c;
			} else {
				// A line longer than the buffer cannot be a message we
				// understand. Drop the whole line rather than acting on its
				// prefix, which would be a truncated command.
				vc.inLen = 0;
				vc.in[0] = '\0';
			}
		}
	}

	if ( got < 0 ) {
		CL_Voice_SetState( VOICE_FAILED, "the helper exited" );
		Sys_StopProcess( vc.proc );
		vc.proc = NULL;
		vc.pending = 0;
	}
}

/*
================
What she is told about the match.

Both of these arrive from cgame as console commands carrying hex -- see
code/cf_cgame/cg_voice.c. Deciding that stats[7] is her health is game
knowledge, and the engine does not have it: cf_shared.h and id's bg_public.h
both define statIndex_t and STAT_HEALTH with different meanings, so the engine
literally cannot see catfight's. The compiler refusing to build the first
attempt is what located the layering line.

From here it is opaque text. cgame knows WHAT happened; the engine decides
whether this is a moment for talking. That is the same split as everywhere
else in this design.
================
*/
void CL_Voice_State( const char *situation ) {
	static char	hex[MAX_VOICE_SITUATION * 2 + 1];
	static char	line[MAX_VOICE_STATE_LINE];

	if ( !situation || !situation[0] ) {
		return;
	}

	/*
	Recorded and logged BEFORE the helper is checked, deliberately.

	"Is the feed right?" and "is the helper up?" are different questions, and
	answering the first should not require the second: cgame builds this every
	second whether or not anybody is listening, and a developer looking at why
	she said something odd about the score needs to see it even on a machine
	with no model installed. Logged only at developer level -- it changes about
	once a second.
	*/
	/*
	Bracketed by markers rather than just introduced by one. The situation is
	many lines of prose with no fixed last line, so without a closing marker
	nothing reading the log can tell where it stops -- whatever the engine
	printed next simply looks like more situation. netplay/test-voicestate.ps1
	reads these blocks out of qconsole.log to assert what she was told, and a
	block that runs on into the next server command is a check that passes or
	fails on unrelated chatter.
	*/
	Com_DPrintf( "voice: situation ->\n%svoice: situation <-\n", situation );
	Q_strncpyz( vc.situation, situation, sizeof( vc.situation ) );

	if ( !CL_Voice_Available() ) {
		return;
	}

	// Static rather than on the stack: this is eight kilobytes of buffer once a
	// second, and it is the same every time.
	CL_Voice_HexEncode( situation, hex, sizeof( hex ) );
	Com_sprintf( line, sizeof( line ), "state %s", hex );
	CL_Voice_Send( line );

	vc.stateSent++;
	vc.lastStateAt = cls.realtime;
}

/*
================
CL_Voice_Event

What the match did to the relationship.

Separate from CL_Voice_Note, and the separation is load-bearing. A note is
prose she may remark on, gated behind a cooldown and an appetite cvar, and
dropping one costs nothing. An event is a fact the helper does arithmetic
with -- how many matches they have played, whether he is on a losing run --
and it must arrive whether or not she happens to be talkative today. So it
takes none of Note's gates, waits on nothing, and is never suppressed by
cl_voiceChat.

The engine still does not know what any of it MEANS. `kind` is an opaque
string from cgame and this file has no table of them, for the same reason it
has no persona: what a match is worth is the companion's business and it lives
on the other side of the pipe.
================
*/
void CL_Voice_Event( const char *kind, const char *detail ) {
	char	kindHex[64 * 2 + 1];
	char	detailHex[MAX_VOICE_TEXT * 2 + 1];
	char	line[MAX_VOICE_LINE];

	if ( !kind || !kind[0] || !CL_Voice_Available() ) {
		return;
	}
	CL_Voice_HexEncode( kind, kindHex, sizeof( kindHex ) );

	if ( detail && detail[0] ) {
		CL_Voice_HexEncode( detail, detailHex, sizeof( detailHex ) );
		Com_sprintf( line, sizeof( line ), "event %s %s", kindHex, detailHex );
	} else {
		Com_sprintf( line, sizeof( line ), "event %s", kindHex );
	}
	CL_Voice_Send( line );

	Com_DPrintf( "voice: event %s %s\n", kind, detail ? detail : "" );
}

void CL_Voice_Note( const char *event ) {
	if ( !event || !event[0] || !CL_Voice_Available() ) {
		return;
	}
	if ( !cl_voiceChat->integer || !cl_voiceNotice->integer ) {
		return;
	}
	// Busy answering him. His question outranks her observation.
	if ( vc.pending ) {
		return;
	}
	if ( cls.realtime - vc.lastNoteAt < VOICE_NOTE_COOLDOWN ) {
		return;
	}

	vc.lastNoteAt = cls.realtime;
	// Nothing to fall back to: she was not asked anything, so a reply of "not
	// an order" must not turn into a second question.
	vc.utterance[0] = '\0';
	CL_Voice_Ask( "note", event, CL_Voice_SayPath(), VOICE_SAY_TIMEOUT );
	Com_DPrintf( "voice: unprompted -- %s\n", event );
}
/*
================
CL_Voice_BetweenMatches

Tell her there is no match on.

THE ONE PIECE OF THE SITUATION ONLY THE ENGINE CAN KNOW. Everywhere else this
file is careful to hold no game knowledge and to pass cgame's sentences through
untouched -- but cgame does not exist when there is no match to describe, so
the moment the player returns to the home screen the feed simply stops, and the
last thing she was told is still a live round.

That is the exact failure the live feed exists to end: a companion improvising
around a fiction that has stopped being true. It became reachable the moment
push-to-talk started working at the menu, because until then there was nothing
to say to her there.

Sent whenever the situation CHANGES, and only once the helper is actually up --
otherwise a player sitting at the home screen while she is still loading would
have the one send land on a helper that was not listening, and she would spend
the whole session describing a match that ended before she was born.

Change-driven rather than once-per-return because there is more than one thing
to be doing out here. Sitting at the home screen, waiting in the matchmaking
queue and connecting to a match that has just been found are three different
situations, and she was being told the first one in all three cases -- so a
player who asked "how long is this going to take?" while queuing was answered
by somebody who did not know he was queuing.

EVERY ONE OF THEM STATES THE DANGER EXPLICITLY, in the same words and the same
place cgame uses. Out here the answer is always "none", and it still has to be
said: told only that they are at home, a model that knows this is a shooter
will happily invent a reason to be tense.
================
*/
static void CL_Voice_BetweenMatches( void ) {
	const char	*mm;
	int			phase;

	if ( clc.state == CA_ACTIVE ) {
		// cgame owns the situation whenever there is one.
		vc.menuPhase = VOICE_MENU_NONE;
		return;
	}
	if ( !CL_Voice_Available() ) {
		return;
	}

	/*
	Read off mm_state rather than tracked here, so this cannot disagree with
	what the menu is showing him. The matchmaker's states are "idle",
	"searching", "found" and "error"; only the middle two are distinguishable
	situations to be in, and everything else is simply being at home.
	*/
	mm = Cvar_VariableString( "mm_state" );
	if ( !Q_stricmp( mm, "searching" ) ) {
		phase = VOICE_MENU_SEARCHING;
	} else if ( !Q_stricmp( mm, "found" ) ) {
		phase = VOICE_MENU_FOUND;
	} else {
		phase = VOICE_MENU_HOME;
	}

	if ( vc.menuPhase == phase
		 && cls.realtime - vc.menuSentAt < VOICE_MENU_RESEND ) {
		return;
	}
	vc.menuPhase = phase;
	vc.menuSentAt = cls.realtime;

	switch ( phase ) {
		case VOICE_MENU_SEARCHING:
			CL_Voice_State(
				"=== RIGHT NOW ===\n"
				"No match on yet. He is in the MATCHMAKING QUEUE, waiting for a "
				"game to be found.\n"
				"DANGER: none. Waiting in a queue is not a fight. There is no "
				"map, no enemy and nothing to react to -- the two of you are "
				"still at home, he is just staring at a menu waiting for it to "
				"say something.\n"
				"A match could start at any moment, so this is a short wait "
				"rather than an evening in.\n" );
			break;

		case VOICE_MENU_FOUND:
			CL_Voice_State(
				"=== RIGHT NOW ===\n"
				"A match has just been FOUND and he is connecting to it.\n"
				"DANGER: none yet. Nothing has started -- there will be warmup "
				"first, and nobody can be hurt for real until a round goes "
				"live.\n"
				"The next few seconds are the last quiet ones before a game.\n" );
			break;

		default:
			CL_Voice_State(
				"=== RIGHT NOW ===\n"
				"There is no match on. The two of you are at home, between "
				"games.\n"
				"DANGER: none. Nobody is shooting at anybody, there is no map "
				"loaded and nothing needs deciding. This is the safest either "
				"of you ever is.\n"
				"He is sitting at the menu, so he has time to talk properly.\n" );
			break;
	}
}

void CL_Voice_Frame( void ) {
	/*
	Before the early return, so the HUD is told which key talks to her even on a
	machine where she never starts. The hint is drawn only when she is actually
	up, but "what is this bound to" and "is she running" are separate questions
	and answering the first should not depend on the second -- the same split
	CL_Voice_State already makes for the situation feed.
	*/
	CL_Voice_PublishKey();

	if ( !vc.proc ) {
		return;
	}

	CL_Voice_Pump();

	if ( !vc.proc ) {
		return;	// the pump noticed it died
	}

	CL_Voice_BetweenMatches();

	if ( vc.state == VOICE_STARTING
		 && cls.realtime - vc.startedAt > VOICE_HELLO_TIMEOUT ) {
		CL_Voice_SetState( VOICE_FAILED, "the helper did not answer" );
	}

	CL_Voice_PumpPlayback();

	if ( vc.recording ) {
		CL_Voice_PumpCapture();
		// A held key that is never released -- an alt-tab mid-sentence is the
		// usual way -- must not record forever. Send what there is and stop.
		if ( vc.clipSamples >= VOICE_MAX_SAMPLES ) {
			Com_DPrintf( "voice: hit the %d second cap\n", VOICE_MAX_SECONDS );
			CL_Voice_FinishRecording();
		}
	}

	if ( vc.pending && cls.realtime - vc.askedAt > vc.pendingTimeout ) {
		/*
		Silence is indistinguishable from a broken pipeline, and COMMANDS.md is
		explicit that the player's first question is always "did she hear me?".
		Say so rather than leaving him wondering.

		NAME THE VERB AND THE WAIT. This line used to say only that she had not
		answered, and the three things it can mean have nothing to do with each
		other: `stt` is speech recognition, `cmd` is the model classifying an
		order, `say` is the model writing a sentence -- different subsystems,
		different timeouts, different fixes. Reported without the verb they are
		one symptom with three causes, which is exactly the shape of bug this
		project keeps paying for. The elapsed time distinguishes "it hit the
		ceiling" from "the helper died and the timer merely expired".
		*/
		Com_Printf( "voice: %s didn't answer in time (%.1fs of %.1fs)\n",
		            vc.pendingWhat[0] ? vc.pendingWhat : "she",
		            ( cls.realtime - vc.askedAt ) / 1000.0f,
		            vc.pendingTimeout / 1000.0f );
		vc.pending = 0;
	}
}

qboolean CL_Voice_Available( void ) {
	return (qboolean)( vc.state == VOICE_READY );
}

/*
================
CL_Voice_Say

Hands one utterance to the helper.

Console command before UI, matching netplay/README.md's standing rule: this is
the mechanism, and speech-to-text is another way to call it rather than a
different path through it.
================
*/
static void CL_Voice_Say_f( void ) {
	char	*text;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: voice_say <what you say to her>\n" );
		return;
	}
	if ( !CL_Voice_Available() ) {
		Com_Printf( "voice: %s\n", vc.error[0] ? vc.error : "the helper is not ready" );
		return;
	}

	text = Cmd_ArgsFrom( 1 );
	if ( strlen( text ) >= MAX_VOICE_TEXT ) {
		Com_Printf( "voice: that is too long to be something you said\n" );
		return;
	}

	// A new utterance supersedes whatever was outstanding. The old reply will
	// still arrive and will be dropped on its sequence number.
	CL_Voice_Utterance( text );
}

/*
================
CL_Voice_PublishKey

Put the name of the key that talks to her into cl_voiceKey, for the HUD.

A CVAR RATHER THAN A SYSCALL, which is the same call Phase 3 made for
matchmaking and for the same reason: cgame wants to draw "hold B to talk to
her", the engine is the only thing that knows what B is, and a cvar crosses
that gap with no new engine surface at all. cgame has trap_Key_GetCatcher and
trap_Key_IsDown and nothing that turns a binding into a key or a key into a
name, so the alternative was two new syscalls for one line of text.

ASKED FOR REPEATEDLY RATHER THAN SET ONCE, because a binding can change from
four places -- the controls menu, cf_bind, a raw `bind` at the console, and the
version migration at startup -- and three of them have no idea this cvar
exists. A second of staleness on a hint nobody is reading yet costs nothing;
being permanently wrong about which key to press is the whole failure this is
meant to prevent.

Empty when the action is bound to nothing, which cgame shows differently on
purpose. That is not a hypothetical state: the migration deliberately refuses
to take a default key that is already in use, and an unbound push-to-talk is
otherwise completely invisible.
================
*/
#define VOICE_KEY_INTERVAL	1000

static void CL_Voice_PublishKey( void ) {
	int key;

	if ( cls.realtime - vc.keyCheckedAt < VOICE_KEY_INTERVAL ) {
		return;
	}
	vc.keyCheckedAt = cls.realtime;

	key = Key_GetKey( "+voicerecord" );

	Cvar_Set( "cl_voiceKey", key >= 0 ? Key_KeynumToString( key ) : "" );
}

/*
================
+voicerecord / -voicerecord

Push to talk. Held, not toggled, so the engine knows exactly when speech begins
and ends and needs no voice activity detection to find the edges of it.

It also fixes the trap COMMANDS.md records about deixis: the moment the key
goes DOWN is when the player meant "there", and that is a full round trip
before the transcript exists. The aim point is not latched here yet -- see
VOICE.md -- but this is the event it will have to be latched on, which is why
the down edge is a command of its own rather than an implementation detail of
the up edge.
================
*/
static void CL_Voice_RecordStart_f( void ) {
	if ( !CL_Voice_Available() ) {
		Com_Printf( "voice: %s\n", vc.error[0] ? vc.error : "the helper is not ready" );
		return;
	}
	if ( vc.recording ) {
		return;
	}

	vc.clipSamples = 0;
	vc.decimSum = 0;
	vc.decimCount = 0;
	vc.recording = qtrue;
	S_StartCapture();

	// Drop whatever the device had queued from before the key went down, so an
	// utterance never begins with the end of the last one.
	CL_Voice_PumpCapture();
	vc.clipSamples = 0;
	vc.decimSum = 0;
	vc.decimCount = 0;
}

static void CL_Voice_RecordStop_f( void ) {
	CL_Voice_FinishRecording();
}

/*
================
voice_memory / voice_forget

What she has written down, and the ability to delete any of it.

BOTH HALVES ARE THE FEATURE, and the second is not a concession to anybody.
Memory is only delightful when it is CORRECT, and being able to read it is the
only thing that keeps it correct: a player who can see a wrong entry deletes
it, and a player who cannot see it just concludes she is strange. It happens
also to be the honest answer to the obvious question about a game that writes
down things you said to it.

The engine holds no copy and parses nothing. What she remembers lives in the
helper, on the far side of the pipe, for exactly the reason the persona does.
================
*/
static void CL_Voice_Memory_f( void ) {
	if ( !CL_Voice_Available() ) {
		Com_Printf( "voice: %s\n", vc.error[0] ? vc.error : "the helper is not ready" );
		return;
	}
	vc.memoryToConsole = qtrue;
	vc.memoryAskedAt = cls.realtime;
	CL_Voice_Send( "memory" );
}

/*
================
CL_Voice_MemoryText

The last listing, for the stats screen.

Returns what it has straight away and asks for a fresh one behind that, at most
once a second. The menu redraws every frame and none of this changes faster
than a match ends, so a round trip per frame would buy nothing and put a pipe
in the render loop.

An empty string is the honest answer before the first reply arrives -- and
while the helper is down, which is a state the screen has to be able to show
rather than hide behind stale text.
================
*/
int CL_Voice_MemoryText( char *buf, int size ) {
	if ( !buf || size <= 0 ) {
		return 0;
	}
	Q_strncpyz( buf, vc.memoryText, size );

	if ( CL_Voice_Available()
		 && cls.realtime - vc.memoryAskedAt > VOICE_MEMORY_REFRESH ) {
		vc.memoryAskedAt = cls.realtime;
		CL_Voice_Send( "memory" );
	}
	return (int)strlen( buf );
}

/*
================
CL_Voice_Subtitle

The last thing she said, and a counter that moves when it changes.

cgame calls this once a frame and compares the counter with the one it saw
last. That comparison is the entire interface: a changed counter means a new
line, an unchanged one means keep drawing whatever is already up, and zero
means she has not spoken at all this session.

Returning the counter rather than a timestamp is deliberate; see the note on
vc.subtitle. Deaf players are the reason this exists at all, so the text is
handed over whether or not there was ever any audio to go with it.
================
*/
int CL_Voice_Subtitle( char *buf, int size ) {
	if ( buf && size > 0 ) {
		Q_strncpyz( buf, vc.subtitle, size );
	}
	return vc.subtitleSeq;
}

static void CL_Voice_Forget_f( void ) {
	char	hex[MAX_VOICE_TEXT * 2 + 1];
	char	line[MAX_VOICE_LINE];

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: voice_forget <number from voice_memory> | facts | matches | all\n" );
		return;
	}
	if ( !CL_Voice_Available() ) {
		Com_Printf( "voice: %s\n", vc.error[0] ? vc.error : "the helper is not ready" );
		return;
	}

	// No confirmation, on purpose. The whole value of this control is that it
	// is believed, and a delete that argues with him is one he stops trusting.
	CL_Voice_HexEncode( Cmd_Argv( 1 ), hex, sizeof( hex ) );
	Com_sprintf( line, sizeof( line ), "forget %s", hex );
	CL_Voice_Send( line );
}

/*
================
voice_tone

How would she take it if you said this?

A tuning command. Whether she is friendly or flirting is arithmetic everywhere
except one judgement call -- reading what the player just said -- so that read
is the only part of the mechanism that can be wrong in an interesting way, and
this is how to look at it without playing a match to find out.

Sent without a sequence number of its own concern: the reply is printed and
nothing acts on it.
================
*/
static void CL_Voice_Tone_f( void ) {
	char	hex[MAX_VOICE_TEXT * 2 + 1];
	char	line[MAX_VOICE_LINE];
	char	*text;

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: voice_tone <something you might say to her>\n" );
		return;
	}
	if ( !CL_Voice_Available() ) {
		Com_Printf( "voice: %s\n", vc.error[0] ? vc.error : "the helper is not ready" );
		return;
	}

	text = Cmd_ArgsFrom( 1 );
	if ( strlen( text ) >= MAX_VOICE_TEXT ) {
		Com_Printf( "voice: that is too long to be something you said\n" );
		return;
	}

	CL_Voice_HexEncode( text, hex, sizeof( hex ) );
	Com_sprintf( line, sizeof( line ), "tone 0 %s", hex );
	CL_Voice_Send( line );
}

static void CL_Voice_Status_f( void ) {
	const char *what;

	switch ( vc.state ) {
		case VOICE_OFF:			what = "off";			break;
		case VOICE_STARTING:	what = "starting";		break;
		case VOICE_READY:		what = "ready";			break;
		default:				what = "unavailable";	break;
	}

	Com_Printf( "helper   : %s%s\n", cl_voiceHelper->string,
				vc.proc ? "" : " (not running)" );
	Com_Printf( "state    : %s\n", what );
	if ( vc.model[0] ) {
		Com_Printf( "model    : %s\n", vc.model );
	}
	Com_Printf( "chat     : %s\n", cl_voiceChat->integer ? "on" : "off" );
	if ( vc.stateSent ) {
		char	copy[MAX_VOICE_SITUATION];
		char	*at, *next;

		Com_Printf( "situation: %i updates, last %i ms ago\n", vc.stateSent,
					cls.realtime - vc.lastStateAt );
		// Printed a line at a time, and on a copy, because splitting it walks
		// over the newlines. Com_Printf would drop most of it in one go.
		Q_strncpyz( copy, vc.situation, sizeof( copy ) );
		for ( at = copy; *at; at = next ) {
			next = strchr( at, '\n' );
			if ( next ) {
				*next++ = '\0';
			} else {
				next = at + strlen( at );
			}
			Com_Printf( "  %s\n", at );
		}
	} else {
		Com_Printf( "situation: " S_COLOR_YELLOW "never sent -- she knows nothing "
					"about the match\n" );
	}
	if ( vc.recording ) {
		Com_Printf( "mic      : recording, %.1fs\n",
					(float)vc.clipSamples / VOICE_RATE_OUT );
	}
	if ( vc.pending ) {
		Com_Printf( "waiting  : %s %d, %d ms\n", vc.pendingWhat, vc.pending,
					cls.realtime - vc.askedAt );
	}
	if ( vc.error[0] ) {
		Com_Printf( "note     : %s\n", vc.error );
	}
}

void CL_Voice_Init( void ) {
	Com_Memset( &vc, 0, sizeof( vc ) );

	// LATCH: the helper is spawned once at startup, so flipping this mid-run
	// would describe a state that is not true.
	cl_voice = Cvar_Get( "cl_voice", "1", CVAR_ARCHIVE | CVAR_LATCH );
	cl_voiceHelper = Cvar_Get( "cl_voiceHelper", "cfvoice",
							   CVAR_ARCHIVE | CVAR_LATCH );
	cl_voiceState = Cvar_Get( "cl_voiceState", "off", CVAR_ROM );
	// ROM: this reports what the bindings already say. Setting it would change
	// nothing, and a player who tried would be told the wrong key by a cvar
	// that looks authoritative.
	cl_voiceKey = Cvar_Get( "cl_voiceKey", "", CVAR_ROM );
	// Not latched: whether she talks back is a preference somebody may want to
	// change mid-session, and unlike the helper itself it describes no state
	// that has to be true at boot. It does NOT disable the companion -- orders
	// still work with this off, because turning her off entirely is not a
	// supported configuration. See VOICE.md.
	cl_voiceChat = Cvar_Get( "cl_voiceChat", "1", CVAR_ARCHIVE );
	// Not archived: keeping recordings of somebody's microphone is a thing you
	// turn on deliberately for a session, never a thing that stays on because
	// it was on once.
	cl_voiceKeep = Cvar_Get( "cl_voiceKeep", "0", CVAR_TEMP );
	// Whether she is allowed to speak first at all. Separate from cl_voiceChat
	// because "answer me when I talk to you" and "pipe up on your own" are
	// different appetites, and a player may well want one without the other.
	cl_voiceNotice = Cvar_Get( "cl_voiceNotice", "1", CVAR_ARCHIVE );
	/*
	How loud she is, on top of s_volume.

	Her line is a raw stream and both mixers scale a raw stream by s_volume
	(S_Base_RawSamples, S_AL_RawSamples), so this is a sub-mix under the
	effects volume rather than an absolute level -- which is the right shape
	for it: she is a voice in the game, not a second game.

	It gets its own slider because she talks over the same fight that is making
	all the other noise, and the two complaints we expect -- "I cannot hear
	what she said" and "she will not shut up" -- are both unfixable with one
	volume control.
	*/
	cl_voiceVolume = Cvar_Get( "cl_voiceVolume", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( cl_voiceVolume, 0, 1, qfalse );

	Cmd_AddCommand( "voice_say", CL_Voice_Say_f );
	Cmd_AddCommand( "voice_status", CL_Voice_Status_f );
	Cmd_AddCommand( "voice_memory", CL_Voice_Memory_f );
	Cmd_AddCommand( "voice_forget", CL_Voice_Forget_f );
	Cmd_AddCommand( "voice_tone", CL_Voice_Tone_f );
	Cmd_AddCommand( "+voicerecord", CL_Voice_RecordStart_f );
	Cmd_AddCommand( "-voicerecord", CL_Voice_RecordStop_f );

	CL_Voice_SetState( VOICE_OFF, NULL );

	if ( !cl_voice->integer ) {
		return;
	}

	// Started at boot rather than on demand: loading the weights takes seconds,
	// and doing it when the player first speaks would make her feel broken the
	// one time first impressions are being formed.
	vc.proc = Sys_StartProcess( cl_voiceHelper->string );
	if ( !vc.proc ) {
		CL_Voice_SetState( VOICE_OFF, "no companion helper in this build" );
		return;
	}

	vc.startedAt = cls.realtime;
	CL_Voice_SetState( VOICE_STARTING, NULL );
	CL_Voice_Send( "hello 1" );
}

void CL_Voice_Shutdown( void ) {
	if ( vc.recording ) {
		// Not FinishRecording: the helper is about to be told to quit, so
		// sending it a clip it will never answer is pointless. Just let the
		// device go.
		S_StopCapture();
		vc.recording = qfalse;
	}
	CL_Voice_StopPlayback();
	if ( vc.proc ) {
		CL_Voice_Send( "quit" );
		Sys_StopProcess( vc.proc );
		vc.proc = NULL;
	}
	Cmd_RemoveCommand( "voice_say" );
	Cmd_RemoveCommand( "voice_status" );
	Cmd_RemoveCommand( "voice_memory" );
	Cmd_RemoveCommand( "voice_forget" );
	Cmd_RemoveCommand( "voice_tone" );
	Cmd_RemoveCommand( "+voicerecord" );
	Cmd_RemoveCommand( "-voicerecord" );
	CL_Voice_SetState( VOICE_OFF, NULL );
}
