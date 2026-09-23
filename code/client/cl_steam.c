/*
===========================================================================
catfight

The engine's side of the Steam helper.

READ THIS BEFORE EDITING. There is no Steamworks header, no Steamworks symbol
and no Steamworks data structure in this file, and there must never be one.
catfight's engine is a fork of ioquake3 and is GPLv2; the Steamworks SDK is
proprietary and Valve's own distributing_opensource page says an application
combining the two must not be distributed on Steam. The whole integration is
therefore a SEPARATE PROGRAM -- see steam/README.md -- and this file talks to
it the way it would talk to any other program: it spawns it and exchanges
lines of text.

Everything below is deliberately written in terms of "the helper", never in
terms of Steam, because that is the actual contract. If a future change here
needs to know something Steam-shaped, the answer is a new line in the protocol,
not an include.

PROTOCOL v1, one message per line, LF-terminated. Full text in steam/README.md.

    ->  hello 1                     us, immediately after spawning
    <-  ready <steamid64> <hex>     helper: signed in; hex is the persona name
    <-  unavailable <text>          helper: no Steam, or nobody signed in

    ->  ticket                      us, when cfmm needs a credential
    <-  ticket <hex>                helper: a web-api ticket, hex encoded
    <-  ticketerror <text>          helper: could not get one

    ->  quit                        us, on shutdown

Unrecognised lines are ignored rather than treated as errors, so a newer helper
can say more without breaking an older engine.
===========================================================================
*/

#include "client.h"

// MAX_STEAM_TICKET is in client.h: cl_mm.c has to size a request body around it.

// The helper only has to reach a local Steam client for these, so they are not
// generous, and a hung helper should be reported rather than waited on.
#define STEAM_HELLO_TIMEOUT		8000
// A ticket is a round trip through Steam's backend, so it gets longer.
#define STEAM_TICKET_TIMEOUT	20000

typedef enum {
	HELPER_OFF,			// disabled, or no helper binary present
	HELPER_STARTING,	// spawned, hello sent, waiting for a reply
	HELPER_READY,		// signed in, can mint tickets
	HELPER_FAILED		// gone, or told us it cannot help
} helperState_t;

typedef struct {
	sysProcess_t	*proc;
	helperState_t	state;
	int				startedAt;

	steamTicketState_t	ticketState;
	int					ticketAskedAt;
	char				ticket[MAX_STEAM_TICKET];

	char			steamID[24];			// 17 digits, decimal
	char			persona[MAX_NAME_LENGTH];
	char			error[192];

	// Partial line accumulator. The pipe hands us arbitrary chunks; messages
	// are lines.
	char			in[MAX_STEAM_TICKET + 256];
	int				inLen;
} steamClient_t;

static steamClient_t	st;

static cvar_t	*cl_steam;
static cvar_t	*cl_steamHelper;
static cvar_t	*cl_steamState;

static void CL_Steam_SetState( helperState_t state, const char *text ) {
	st.state = state;

	switch ( state ) {
		case HELPER_OFF:		Cvar_Set( "cl_steamState", "off" );			break;
		case HELPER_STARTING:	Cvar_Set( "cl_steamState", "starting" );	break;
		case HELPER_READY:		Cvar_Set( "cl_steamState", "ready" );		break;
		default:				Cvar_Set( "cl_steamState", "failed" );		break;
	}

	if ( text ) {
		Q_strncpyz( st.error, text, sizeof( st.error ) );
	} else {
		st.error[0] = '\0';
	}
}

/*
================
CL_Steam_HexDecode

The helper hex-encodes any text it sends us, for the same reason the match
ticket hex-encodes the display name: a persona name is arbitrary user text, and
this protocol is space-separated. A space or a newline in a name would
otherwise split one message into two.

Returns qfalse and leaves out empty on anything that is not clean hex.
================
*/
static int CL_Steam_HexDigit( char c ) {
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

static qboolean CL_Steam_HexDecode( const char *hex, char *out, int outSize ) {
	int	len = (int)strlen( hex );
	int	i, o = 0;

	out[0] = '\0';

	if ( len & 1 ) {
		return qfalse;
	}
	for ( i = 0; i < len; i += 2 ) {
		int hi = CL_Steam_HexDigit( hex[i] );
		int lo = CL_Steam_HexDigit( hex[i + 1] );

		if ( hi < 0 || lo < 0 ) {
			out[0] = '\0';
			return qfalse;
		}
		if ( o >= outSize - 1 ) {
			break;	// truncate rather than overrun; a long name is not an error
		}
		out[o++] = (char)( ( hi << 4 ) | lo );
	}
	out[o] = '\0';
	return qtrue;
}

static void CL_Steam_Send( const char *line ) {
	char	buf[128];
	int		len;

	if ( !st.proc ) {
		return;
	}
	len = Com_sprintf( buf, sizeof( buf ), "%s\n", line );
	if ( Sys_ProcessWrite( st.proc, buf, len ) < 0 ) {
		CL_Steam_SetState( HELPER_FAILED, "the helper stopped listening" );
	}
}

/*
================
CL_Steam_Message

One complete line from the helper.
================
*/
static void CL_Steam_Message( char *line ) {
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
		char	*personaHex = strchr( rest, ' ' );

		if ( personaHex ) {
			*personaHex++ = '\0';
			CL_Steam_HexDecode( personaHex, st.persona, sizeof( st.persona ) );
		}
		Q_strncpyz( st.steamID, rest, sizeof( st.steamID ) );
		CL_Steam_SetState( HELPER_READY, NULL );
		Com_DPrintf( "steam: helper ready (%s)\n", st.steamID );
		return;
	}

	if ( !strcmp( verb, "unavailable" ) ) {
		// Not a failure of ours and not worth a warning colour: playing without
		// Steam is a supported thing to do.
		CL_Steam_SetState( HELPER_FAILED, rest[0] ? rest : "Steam is not running" );
		Com_DPrintf( "steam: unavailable -- %s\n", st.error );
		return;
	}

	if ( !strcmp( verb, "ticket" ) ) {
		if ( strlen( rest ) >= sizeof( st.ticket ) ) {
			// Refuse rather than store a truncated ticket. A truncated one
			// would be rejected by Valve with an error that blames the
			// credential, which is exactly the wrong place to go looking.
			st.ticketState = STEAMTICKET_FAILED;
			Q_strncpyz( st.error, "the helper sent a ticket too large to hold",
						sizeof( st.error ) );
			return;
		}
		Q_strncpyz( st.ticket, rest, sizeof( st.ticket ) );
		st.ticketState = STEAMTICKET_READY;
		return;
	}

	if ( !strcmp( verb, "ticketerror" ) ) {
		st.ticketState = STEAMTICKET_FAILED;
		Q_strncpyz( st.error, rest[0] ? rest : "could not get a Steam ticket",
					sizeof( st.error ) );
		return;
	}

	// Anything else is from a newer helper than this engine. Ignoring it is
	// what makes the protocol extensible in the direction that matters.
}

/*
================
CL_Steam_Pump

Drains whatever the pipe has and dispatches complete lines.
================
*/
static void CL_Steam_Pump( void ) {
	char	buf[1024];
	int		got, i;

	while ( ( got = Sys_ProcessRead( st.proc, buf, sizeof( buf ) ) ) > 0 ) {
		for ( i = 0; i < got; i++ ) {
			char c = buf[i];

			if ( c == '\r' ) {
				continue;
			}
			if ( c == '\n' ) {
				st.in[st.inLen] = '\0';
				if ( st.inLen > 0 ) {
					CL_Steam_Message( st.in );
				}
				st.inLen = 0;
				continue;
			}
			if ( st.inLen < (int)sizeof( st.in ) - 1 ) {
				st.in[st.inLen++] = c;
			} else {
				// A line longer than the buffer cannot be a message we
				// understand. Drop the whole line rather than acting on its
				// prefix, which would be a truncated ticket.
				st.inLen = 0;
				st.in[0] = '\0';
			}
		}
	}

	if ( got < 0 ) {
		CL_Steam_SetState( HELPER_FAILED, "the helper exited" );
		Sys_StopProcess( st.proc );
		st.proc = NULL;
		if ( st.ticketState == STEAMTICKET_PENDING ) {
			st.ticketState = STEAMTICKET_FAILED;
		}
	}
}

void CL_Steam_Frame( void ) {
	if ( !st.proc ) {
		return;
	}

	CL_Steam_Pump();

	if ( !st.proc ) {
		return;	// the pump noticed it died
	}

	if ( st.state == HELPER_STARTING
		 && cls.realtime - st.startedAt > STEAM_HELLO_TIMEOUT ) {
		CL_Steam_SetState( HELPER_FAILED, "the helper did not answer" );
	}

	if ( st.ticketState == STEAMTICKET_PENDING
		 && cls.realtime - st.ticketAskedAt > STEAM_TICKET_TIMEOUT ) {
		st.ticketState = STEAMTICKET_FAILED;
		Q_strncpyz( st.error, "timed out waiting for a Steam ticket",
					sizeof( st.error ) );
	}
}

qboolean CL_Steam_Available( void ) {
	return (qboolean)( st.state == HELPER_READY );
}

/*
================
CL_Steam_Starting

The helper has been spawned but has not said yet whether anyone is signed in.

This exists so that `mm_provider auto` is not a RACE. Asking
CL_Steam_Available() during the handshake answers "no" -- truthfully, but only
because the question was early -- and auto would pick guid on a machine that
was about to offer Steam. Every netplay suite passed for exactly that reason:
they run mm_find at startup, before the pipe has answered.

The handshake has its own timeout, so a caller that waits on this always gets
an answer.
================
*/
qboolean CL_Steam_Starting( void ) {
	return (qboolean)( st.state == HELPER_STARTING );
}

const char *CL_Steam_Error( void ) {
	return st.error;
}

const char *CL_Steam_PersonaName( void ) {
	return st.persona;
}

/*
================
CL_Steam_RequestTicket

Asks for a fresh credential. Always fresh: a ticket is bound to the session
that minted it, and holding one across a helper restart would produce a
credential Valve has already forgotten.
================
*/
qboolean CL_Steam_RequestTicket( void ) {
	if ( !CL_Steam_Available() ) {
		return qfalse;
	}
	st.ticket[0] = '\0';
	st.ticketState = STEAMTICKET_PENDING;
	st.ticketAskedAt = cls.realtime;
	CL_Steam_Send( "ticket" );
	return qtrue;
}

steamTicketState_t CL_Steam_TicketState( void ) {
	return st.ticketState;
}

const char *CL_Steam_Ticket( void ) {
	if ( st.ticketState != STEAMTICKET_READY ) {
		return "";
	}
	return st.ticket;
}

static void CL_Steam_Status_f( void ) {
	const char *what;

	switch ( st.state ) {
		case HELPER_OFF:		what = "off";						break;
		case HELPER_STARTING:	what = "starting";					break;
		case HELPER_READY:		what = "ready";						break;
		default:				what = "unavailable";				break;
	}

	Com_Printf( "helper   : %s%s\n", cl_steamHelper->string,
				st.proc ? "" : " (not running)" );
	Com_Printf( "state    : %s\n", what );
	if ( st.steamID[0] ) {
		Com_Printf( "steam id : %s\n", st.steamID );
	}
	if ( st.persona[0] ) {
		Com_Printf( "persona  : %s\n", st.persona );
	}
	if ( st.error[0] ) {
		Com_Printf( "note     : %s\n", st.error );
	}
}

void CL_Steam_Init( void ) {
	Com_Memset( &st, 0, sizeof( st ) );

	// LATCH: the helper is spawned once at startup, so flipping this mid-run
	// would describe a state that is not true.
	cl_steam = Cvar_Get( "cl_steam", "1", CVAR_ARCHIVE | CVAR_LATCH );
	cl_steamHelper = Cvar_Get( "cl_steamHelper", "catfight_steam",
							   CVAR_ARCHIVE | CVAR_LATCH );
	cl_steamState = Cvar_Get( "cl_steamState", "off", CVAR_ROM );

	Cmd_AddCommand( "steam_status", CL_Steam_Status_f );

	CL_Steam_SetState( HELPER_OFF, NULL );

	if ( !cl_steam->integer ) {
		return;
	}

	// Started at boot rather than on demand, so the handshake has long since
	// finished by the time anyone presses PLAY. A helper that is not there is
	// not an error -- a build with no Steam integration is a build that runs.
	st.proc = Sys_StartProcess( cl_steamHelper->string );
	if ( !st.proc ) {
		CL_Steam_SetState( HELPER_OFF, "no Steam helper in this build" );
		return;
	}

	st.startedAt = cls.realtime;
	CL_Steam_SetState( HELPER_STARTING, NULL );
	CL_Steam_Send( "hello 1" );
}

void CL_Steam_Shutdown( void ) {
	if ( st.proc ) {
		CL_Steam_Send( "quit" );
		Sys_StopProcess( st.proc );
		st.proc = NULL;
	}
	Cmd_RemoveCommand( "steam_status" );
	CL_Steam_SetState( HELPER_OFF, NULL );
}
