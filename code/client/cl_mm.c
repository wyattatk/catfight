/*
===========================================================================
catfight

The client half of matchmaking: mm_find, mm_cancel, mm_status.

Phase 3 of netplay/README.md. Console commands before UI, deliberately -- this
makes the whole matchmaking loop testable and scriptable with nothing drawn on
screen, and the UI later becomes presentation over a mechanism that already
works. Everything a UI would need to draw is mirrored into the mm_state and
mm_statusText cvars, so it needs no new syscalls to show a search in progress.
===========================================================================
*/

#include "client.h"
#include "../qcommon/cf_json.h"

#ifdef USE_HTTP

// Where the session token lives. NOT a cvar: an archived cvar lands in
// catfightconfig.cfg, which is a file people paste into forum posts and bug
// reports, and this one is a bearer credential.
#define MM_TOKEN_FILE	"cfaccount"

#define MM_POLL_INTERVAL	2000	// ms between queue polls

// How long "match found" may last without the connect completing. Generous:
// the address has to be reached, the map loaded and the server's ticket check
// passed, and a slow machine on a slow link is not a failure. Anything past
// this is, and the player gets told rather than left watching a spinner.
#define MM_CONNECT_TIMEOUT	20000

typedef enum {
	MM_IDLE,
	MM_PROVIDER,	// waiting for the helper to say whether Steam is there
	MM_TICKET,		// waiting on the Steam helper for a credential
	MM_AUTH,		// POST /v1/auth in flight
	MM_JOINING,		// POST /v1/queue in flight
	MM_QUEUED,		// in the queue, waiting to poll again
	MM_POLLING,		// GET /v1/queue in flight
	MM_CANCELLING,	// DELETE /v1/queue in flight
	MM_NAMING,		// POST /v1/player/name in flight
	MM_LINKING,		// POST /v1/player/steam in flight
	MM_MATCHED
} mmState_t;

// What the Steam ticket we are waiting on is for. A ticket is wanted in two
// unrelated flows -- logging in as a Steam account, and attaching one to the
// account you already are -- and they resume differently.
typedef enum {
	TICKET_FOR_AUTH,
	TICKET_FOR_LINK
} mmTicketUse_t;

typedef struct {
	mmState_t	state;
	int			request;		// in-flight HTTP handle, 0 if none
	qboolean	queueAfterAuth;	// mm_find triggered the auth
	qboolean	nameAfterAuth;	// mm_name triggered the auth
	qboolean	linkAfterAuth;	// mm_linkSteam triggered the auth
	/*
	Forces this login to be the guid account even when Steam is available.

	Only mm_linkSteam sets it, and it is the difference between that command
	working and doing the exact thing it exists to prevent: logging in with a
	Steam credential cfmm has not seen before CREATES A NEW ACCOUNT, so a link
	flow that re-authenticated under `auto` would cheerfully attach Steam to
	the fresh account and leave the old one behind. It has to survive a 401
	retry, which is precisely when this would otherwise bite.
	*/
	qboolean	authAsGuid;
	/*
	This matchmaker will not take a Steam credential, so stop offering one.

	Set only under `auto`, and only after the server has actually said so. It
	is a property of the matchmaker rather than of us, which is why it is
	remembered for the session instead of being retried on every search.
	*/
	qboolean	steamRefused;
	qboolean	triedSteam;		// the auth in flight used a Steam ticket
	char		steamError[192];	// why Steam was refused, kept for the message
	mmTicketUse_t ticketUse;	// meaningful while state is MM_TICKET
	int			nextPoll;		// cls.realtime of the next poll
	int			searchStart;
	int			matchedAt;		// cls.realtime we were handed an address
	char		token[80];
	char		displayName[MAX_NAME_LENGTH];
	char		pendingName[MAX_NAME_LENGTH];	// what mm_name is setting
} mmClient_t;

static mmClient_t	mm;

static cvar_t	*mm_server;
static cvar_t	*mm_https;
static cvar_t	*mm_state;
static cvar_t	*mm_statusText;
static cvar_t	*mm_autoConnect;
static cvar_t	*mm_provider;

/*
================
MM_SetStatus

Mirrors state into cvars. The console commands are the interface today; these
are what a UI will read tomorrow without needing HTTP syscalls of its own.
================
*/
static Q_PRINTF_FUNC(2, 3) void MM_SetStatus( const char *state, const char *fmt, ... ) {
	va_list	argptr;
	char	text[256];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Cvar_Set( "mm_state", state );
	Cvar_Set( "mm_statusText", text );
}

static Q_PRINTF_FUNC(1, 2) void MM_Fail( const char *fmt, ... ) {
	va_list	argptr;
	char	text[256];

	va_start( argptr, fmt );
	Q_vsnprintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	Com_Printf( S_COLOR_YELLOW "matchmaking: %s\n", text );
	MM_SetStatus( "error", "%s", text );

	if ( mm.request ) {
		CL_HTTP_FreeRequest( mm.request );
		mm.request = 0;
	}
	// A failed link leaves nothing to resume, and authAsGuid outliving it would
	// quietly pin every later login to guid.
	mm.linkAfterAuth = qfalse;
	mm.authAsGuid = qfalse;
	mm.state = MM_IDLE;
}

// ---------------------------------------------------------------------------
// token persistence
// ---------------------------------------------------------------------------

static void MM_LoadToken( void ) {
	fileHandle_t	f;
	int				len;
	char			buf[sizeof( mm.token )];

	mm.token[0] = '\0';

	len = FS_BaseDir_FOpenFileRead( MM_TOKEN_FILE, &f );
	if ( !f ) {
		return;
	}
	if ( len <= 0 || len >= (int)sizeof( buf ) ) {
		FS_FCloseFile( f );
		return;
	}

	FS_Read( buf, len, f );
	FS_FCloseFile( f );
	buf[len] = '\0';

	// Trim any trailing newline a human might have left behind.
	while ( len > 0 && ( buf[len - 1] == '\n' || buf[len - 1] == '\r' ) ) {
		buf[--len] = '\0';
	}

	Q_strncpyz( mm.token, buf, sizeof( mm.token ) );
}

static void MM_SaveToken( void ) {
	fileHandle_t f;

	f = FS_BaseDir_FOpenFileWrite_HomeState( MM_TOKEN_FILE );
	if ( !f ) {
		Com_Printf( S_COLOR_YELLOW "matchmaking: could not save %s\n", MM_TOKEN_FILE );
		return;
	}
	FS_Write( mm.token, (int)strlen( mm.token ), f );
	FS_FCloseFile( f );
}

// ---------------------------------------------------------------------------
// requests
// ---------------------------------------------------------------------------

/*
================
MM_URL

mm_server holds host[:port], NOT a full URL, and that is not a style choice.

The engine tokenizer strips `//` as a comment (COM_ParseExt), so a scheme
cannot survive being typed anywhere a cvar is set -- command line, config file
or console alike. `+set mm_server http://127.0.0.1:8080` arrives as `http:`,
and the only symptom is a WinINet 12006 much later, which reads like a network
fault rather than a parsing one.

So the scheme is assembled here and selected by mm_https. A value that somehow
does arrive with a scheme intact is passed through untouched.
================
*/
static const char *MM_URL( const char *path ) {
	const char *base = mm_server->string;
	const char *scheme = mm_https->integer ? "https://" : "http://";
	char host[HTTP_MAX_URL];
	int len;

	if ( strstr( base, "://" ) ) {
		scheme = "";
	}

	Q_strncpyz( host, base, sizeof( host ) );
	len = (int)strlen( host );

	// Tolerate a trailing slash rather than producing a double one.
	while ( len > 0 && host[len - 1] == '/' ) {
		host[--len] = '\0';
	}

	return va( "%s%s%s", scheme, host, path );
}

static const char *MM_AuthHeaders( void ) {
	return va( "Content-Type: application/json\r\nAuthorization: Bearer %s", mm.token );
}

static qboolean MM_Begin( const char *method, const char *path,
						  const char *body, qboolean authed ) {
	mm.request = CL_HTTP_BeginRequest( method, MM_URL( path ), body,
									   authed ? MM_AuthHeaders()
											  : "Content-Type: application/json" );
	if ( !mm.request ) {
		MM_Fail( "could not start request (HTTP unavailable?)" );
		return qfalse;
	}
	return qtrue;
}

/*
================
MM_WantSteam

Which identity provider this client should present.

`auto` prefers Steam when the helper is there and signed in, and falls back to
guid when it is not. That fallback is safe ONLY because the decision that
matters is not made here: cfmm refuses the guid provider outright once Steam is
configured (see -allow-guid), so a client falling back is a client that gets
turned away, not a client that gets in cheaply. Making the CLIENT the authority
would hand anyone with a ban a one-flag way around it.
================
*/
static qboolean MM_WantSteam( void ) {
	if ( mm.authAsGuid ) {
		return qfalse;
	}
	// Only ever set under `auto`, so forcing mm_provider steam still forces it.
	if ( mm.steamRefused ) {
		return qfalse;
	}
	if ( !Q_stricmp( mm_provider->string, "guid" ) ) {
		return qfalse;
	}
	if ( !Q_stricmp( mm_provider->string, "steam" ) ) {
		return qtrue;
	}
	return CL_Steam_Available();
}

/*
================
MM_SendAuth

POSTs the credential. By this point a Steam ticket, if one was wanted, is in
hand -- see MM_StartAuth.
================
*/
static qboolean MM_SendAuth( void ) {
	// Static rather than automatic: a Steam ticket is kilobytes, and this is
	// reached from the frame loop. Nothing re-enters it -- one request is in
	// flight at a time -- so a single buffer is enough.
	static char	body[MAX_STEAM_TICKET + 128];

	mm.triedSteam = qfalse;

	if ( MM_WantSteam() ) {
		const char *ticket = CL_Steam_Ticket();

		if ( !ticket[0] ) {
			MM_Fail( "no Steam ticket: %s", CL_Steam_Error() );
			return qfalse;
		}
		// A ticket is hex, so like cl_playerKey it needs no escaping.
		Com_sprintf( body, sizeof( body ),
					 "{\"provider\":\"steam\",\"credential\":\"%s\"}", ticket );
		mm.triedSteam = qtrue;
	} else {
		const char *key = Cvar_VariableString( "cl_playerKey" );

		if ( !key[0] ) {
			MM_Fail( "no player key -- is the qkey file readable?" );
			return qfalse;
		}
		// cl_playerKey is 32 hex characters, so it needs no escaping.
		Com_sprintf( body, sizeof( body ),
					 "{\"provider\":\"guid\",\"credential\":\"%s\"}", key );
	}

	if ( !MM_Begin( "POST", "/v1/auth", body, qfalse ) ) {
		return qfalse;
	}
	mm.state = MM_AUTH;
	MM_SetStatus( "searching", "identifying" );
	return qtrue;
}

/*
================
MM_StartAuth

Identify to the matchmaker.

Two-step when the credential is a Steam ticket, because minting one is a round
trip through Steam's backend and the helper cannot answer synchronously. The
wait lives in its own state rather than blocking: a frame spent waiting on
another process is a frame the game has stopped drawing.
================
*/
static qboolean MM_StartAuth( void ) {
	/*
	Do not let `auto` resolve while the helper is mid-handshake.

	Asking too early gets a truthful "Steam is not available" that is really
	"not yet", and auto would settle on guid for a session that was about to
	have Steam. It is a race, and it is one that hides: every netplay suite ran
	mm_find at startup and therefore always took the guid branch, which is why
	none of them noticed a Steam-capable client existed at all.
	*/
	if ( CL_Steam_Starting() && !mm.authAsGuid && !mm.steamRefused
		 && !Q_stricmp( mm_provider->string, "auto" ) ) {
		mm.state = MM_PROVIDER;
		MM_SetStatus( "searching", "checking with Steam" );
		return qtrue;
	}

	if ( MM_WantSteam() ) {
		if ( !CL_Steam_Available() ) {
			// Only reachable with mm_provider forced to steam; auto would have
			// chosen guid. Say which of the two things is missing.
			MM_Fail( "Steam is not available: %s", CL_Steam_Error() );
			return qfalse;
		}
		if ( !CL_Steam_RequestTicket() ) {
			MM_Fail( "could not ask the Steam helper for a ticket" );
			return qfalse;
		}
		mm.ticketUse = TICKET_FOR_AUTH;
		mm.state = MM_TICKET;
		MM_SetStatus( "searching", "checking with Steam" );
		return qtrue;
	}
	return MM_SendAuth();
}

static qboolean MM_StartJoin( void ) {
	if ( !MM_Begin( "POST", "/v1/queue", NULL, qtrue ) ) {
		return qfalse;
	}
	mm.state = MM_JOINING;
	MM_SetStatus( "searching", "joining the queue" );
	return qtrue;
}

/*
================
MM_JSONEscape

A display name is the first user-typed text this client ever puts in a JSON
body, and the request is built with sprintf. An unescaped quote would not just
corrupt the name, it would let the player write arbitrary fields into the
request -- so this is the difference between a text field and an injection.

Backslash and quote are escaped; control characters and DEL are dropped rather
than escaped, because none of them belong in a name and \u form would be more
code for no benefit. Bytes above 0x7f pass straight through: they are UTF-8
continuation bytes and JSON takes them verbatim.
================
*/
static void MM_JSONEscape( const char *in, char *out, int outSize ) {
	int i = 0;

	for ( ; *in && i < outSize - 2; in++ ) {
		unsigned char c = (unsigned char)*in;

		if ( c < 0x20 || c == 0x7f ) {
			continue;
		}
		if ( c == '"' || c == '\\' ) {
			out[i++] = '\\';
		}
		out[i++] = (char)c;
	}
	out[i] = '\0';
}

static qboolean MM_StartSetName( void ) {
	char escaped[MAX_NAME_LENGTH * 2 + 2];
	char body[MAX_NAME_LENGTH * 2 + 32];

	MM_JSONEscape( mm.pendingName, escaped, sizeof( escaped ) );
	Com_sprintf( body, sizeof( body ), "{\"name\":\"%s\"}", escaped );

	if ( !MM_Begin( "POST", "/v1/player/name", body, qtrue ) ) {
		return qfalse;
	}
	mm.state = MM_NAMING;
	return qtrue;
}

/*
================
MM_StartLinkSteam

Attaches a Steam account to the account this client already is.

Deliberately NOT the same thing as logging in with Steam. Authenticating with a
Steam credential that cfmm has never seen creates a SECOND player, and
everything earned on the first one -- name, currency, cosmetics -- stays there.
Linking is what keeps a guid account from being orphaned the day Steam arrives,
which is the whole reason it exists as a separate endpoint.
================
*/
static qboolean MM_StartLinkSteam( void ) {
	static char	body[MAX_STEAM_TICKET + 128];
	const char	*ticket = CL_Steam_Ticket();

	if ( !ticket[0] ) {
		MM_Fail( "no Steam ticket: %s", CL_Steam_Error() );
		return qfalse;
	}
	Com_sprintf( body, sizeof( body ), "{\"credential\":\"%s\"}", ticket );

	if ( !MM_Begin( "POST", "/v1/player/steam", body, qtrue ) ) {
		return qfalse;
	}
	mm.state = MM_LINKING;
	MM_SetStatus( "idle", "linking your Steam account" );
	return qtrue;
}

// ---------------------------------------------------------------------------
// responses
// ---------------------------------------------------------------------------

/*
================
MM_HandleAssignment

Shared by the join and poll responses: cfmm hands back an assignment from
either, since queueing when you are already in a match returns the match rather
than queueing you twice.
================
*/
static void MM_HandleAssignment( const char *json ) {
	char	connect[128];
	// Was 80, which fitted the pre-4c ticket exactly. The signed display name
	// adds up to 64 hex characters, and a ticket silently truncated here would
	// fail its signature check on the server and read as "invalid match ticket"
	// -- a message pointing at the secret rather than at this buffer.
	char	ticket[192];
	char	scope[16];
	int		team = 0;

	if ( !CF_JSON_GetString( json, "connect", connect, sizeof( connect ) ) || !connect[0] ) {
		MM_Fail( "matched but no address in the reply" );
		return;
	}
	CF_JSON_GetString( json, "ticket", ticket, sizeof( ticket ) );
	CF_JSON_GetString( json, "scope", scope, sizeof( scope ) );
	CF_JSON_GetInt( json, "team", &team );

	// The ticket rides in userinfo so the server can check it. Nothing
	// validates it until Phase 4 -- plumbing it now means Phase 4 is a
	// server-side change only.
	Cvar_Set( "cl_matchTicket", ticket );

	mm.state = MM_MATCHED;
	mm.matchedAt = cls.realtime;
	MM_SetStatus( "found", "match found on %s", connect );

	Com_Printf( "matchmaking: match found -- %s (%s address), team %d\n",
				connect, scope[0] ? scope : "?", team );

	if ( mm_autoConnect->integer ) {
		Cbuf_AddText( va( "connect %s\n", connect ) );
	} else {
		Com_Printf( "matchmaking: mm_autoConnect is 0; connect %s\n", connect );
	}
}

/*
================
MM_HandleQueueState

Handles the body of a join or poll response, both of which return the same
shape: {"state":"idle"|"queued"|"matched", ...}.
================
*/
static void MM_HandleQueueState( const char *json ) {
	char	state[16];
	char	reason[16];
	int		waiting = 0;
	int		position = 0;

	if ( !CF_JSON_GetString( json, "state", state, sizeof( state ) ) ) {
		MM_Fail( "unexpected reply from the matchmaker" );
		return;
	}

	if ( !Q_stricmp( state, "matched" ) ) {
		MM_HandleAssignment( json );
		return;
	}

	if ( !Q_stricmp( state, "queued" ) ) {
		CF_JSON_GetInt( json, "waiting", &waiting );
		CF_JSON_GetInt( json, "position", &position );
		reason[0] = '\0';
		CF_JSON_GetString( json, "reason", reason, sizeof( reason ) );

		mm.state = MM_QUEUED;
		mm.nextPoll = cls.realtime + MM_POLL_INTERVAL;

		if ( waiting <= 0 ) {
			waiting = ( cls.realtime - mm.searchStart ) / 1000;
		}

		// Waiting for an opponent and waiting for a free server are different
		// problems with the same symptom, and a player who cannot tell them
		// apart concludes the game is broken. The matchmaker knows which it is,
		// so say it.
		if ( !Q_stricmp( reason, "capacity" ) && position > 0 ) {
			MM_SetStatus( "searching", "all servers busy -- #%d in line (%ds)",
						  position, waiting );
		} else {
			MM_SetStatus( "searching", "searching for an opponent (%ds)", waiting );
		}
		return;
	}

	// "idle" -- somebody or something took us out of the queue.
	mm.state = MM_IDLE;
	MM_SetStatus( "idle", "not searching" );
}

/*
================
MM_HandleError

Turns a non-2xx into something a player can act on. A 401 is special: the token
we saved is no longer valid, so drop it and start again rather than looping.
================
*/
static void MM_HandleError( int code, const char *json ) {
	char detail[192];

	if ( !CF_JSON_GetString( json, "error", detail, sizeof( detail ) ) ) {
		Q_strncpyz( detail, "no detail", sizeof( detail ) );
	}

	if ( code == 401 ) {
		qboolean retry = ( mm.state != MM_AUTH );

		mm.token[0] = '\0';
		if ( mm.request ) {
			CL_HTTP_FreeRequest( mm.request );
			mm.request = 0;
		}

		if ( retry ) {
			Com_DPrintf( "matchmaking: token rejected, re-identifying\n" );
			mm.queueAfterAuth = qtrue;
			MM_StartAuth();
			return;
		}
		MM_Fail( "the matchmaker rejected our identity: %s", detail );
		return;
	}

	/*
	This matchmaker does not take Steam. Under `auto` that is an answer, not a
	failure -- fall back to guid and try once more.

	Safe, and for the reason the whole design rests on: the SERVER decides what
	it accepts. A client cannot talk its way in by claiming Steam is
	unavailable, because a matchmaker with guid switched off refuses the retry
	too. What this buys is that a build with a helper still works against a
	matchmaker that has not had Steam turned on yet -- which is every dev
	machine today, and the entire pool during the 4b rollout.

	The original Steam message is kept: if guid is refused as well, that is the
	error worth showing, not "unknown provider guid".
	*/
	if ( mm.state == MM_AUTH && mm.triedSteam && !mm.steamRefused
		 && !Q_stricmp( mm_provider->string, "auto" ) ) {
		Q_strncpyz( mm.steamError, detail, sizeof( mm.steamError ) );
		mm.steamRefused = qtrue;

		if ( mm.request ) {
			CL_HTTP_FreeRequest( mm.request );
			mm.request = 0;
		}
		Com_DPrintf( "matchmaking: no Steam here (%s), using the local key\n",
					 detail );
		MM_SendAuth();
		return;
	}

	if ( mm.steamRefused && mm.steamError[0] && mm.state == MM_AUTH ) {
		MM_Fail( "the matchmaker refused both identities -- Steam: %s; local key: %s",
				 mm.steamError, detail );
		return;
	}

	MM_Fail( "matchmaker returned %d: %s", code, detail );
}

static void MM_HandleResponse( void ) {
	mmState_t	was = mm.state;
	int			code = CL_HTTP_RequestCode( mm.request );
	const char	*json = CL_HTTP_RequestBody( mm.request );
	char		body[HTTP_MAX_RESPONSE];

	// Copy before freeing: the buffer belongs to the request slot.
	Q_strncpyz( body, json, sizeof( body ) );

	if ( code < 200 || code > 299 ) {
		MM_HandleError( code, body );
		return;
	}

	CL_HTTP_FreeRequest( mm.request );
	mm.request = 0;

	switch ( was ) {
		case MM_AUTH: {
			char token[sizeof( mm.token )];

			if ( !CF_JSON_GetString( body, "token", token, sizeof( token ) ) || !token[0] ) {
				MM_Fail( "no token in the identity reply" );
				return;
			}
			Q_strncpyz( mm.token, token, sizeof( mm.token ) );
			CF_JSON_GetString( body, "display_name", mm.displayName,
							   sizeof( mm.displayName ) );
			MM_SaveToken();

			Com_DPrintf( "matchmaking: identified as %s\n", mm.displayName );

			if ( mm.queueAfterAuth ) {
				mm.queueAfterAuth = qfalse;
				MM_StartJoin();
			} else if ( mm.nameAfterAuth ) {
				mm.nameAfterAuth = qfalse;
				MM_StartSetName();
			} else if ( mm.linkAfterAuth ) {
				mm.linkAfterAuth = qfalse;
				if ( CL_Steam_RequestTicket() ) {
					mm.ticketUse = TICKET_FOR_LINK;
					mm.state = MM_TICKET;
					MM_SetStatus( "idle", "checking with Steam" );
				} else {
					MM_Fail( "Steam is not available: %s", CL_Steam_Error() );
				}
			} else {
				mm.state = MM_IDLE;
				MM_SetStatus( "idle", "not searching" );
			}
			return;
		}

		case MM_NAMING:
			CF_JSON_GetString( body, "display_name", mm.displayName,
							   sizeof( mm.displayName ) );
			mm.state = MM_IDLE;
			MM_SetStatus( "idle", "not searching" );
			Com_Printf( "matchmaking: you are now %s\n", mm.displayName );
			Com_Printf( "this is the name other players see; it takes effect "
						"from your next match\n" );
			return;

		case MM_LINKING: {
			char steamID[24];

			mm.authAsGuid = qfalse;
			mm.state = MM_IDLE;
			MM_SetStatus( "idle", "not searching" );

			CF_JSON_GetString( body, "steam_id", steamID, sizeof( steamID ) );
			Com_Printf( "matchmaking: Steam account %s is now linked to this "
						"catfight account\n", steamID[0] ? steamID : "(unknown)" );
			Com_Printf( "your name, currency and cosmetics are unchanged\n" );
			return;
		}

		case MM_JOINING:
		case MM_POLLING:
			MM_HandleQueueState( body );
			return;

		case MM_CANCELLING:
			mm.state = MM_IDLE;
			MM_SetStatus( "idle", "not searching" );
			Com_Printf( "matchmaking: search cancelled\n" );
			return;

		default:
			mm.state = MM_IDLE;
			return;
	}
}

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void CL_MM_Frame( void ) {
	httpReqState_t reqState;

	CL_HTTP_ReapRequests();

	/*
	The helper has finished its handshake, so `auto` can now be answered.
	Terminates because the handshake has its own timeout -- CL_Steam_Starting
	stops being true either way.
	*/
	if ( mm.state == MM_PROVIDER ) {
		if ( !CL_Steam_Starting() ) {
			MM_StartAuth();
		}
		return;
	}

	/*
	Resume a flow that is waiting on the Steam helper.

	The helper's own timeout (cl_steam.c) is what guarantees this terminates, so
	there is no second deadline here -- two timers on one wait is how you get a
	state that neither of them owns.
	*/
	if ( mm.state == MM_TICKET ) {
		switch ( CL_Steam_TicketState() ) {
			case STEAMTICKET_READY:
				if ( mm.ticketUse == TICKET_FOR_LINK ) {
					MM_StartLinkSteam();
				} else {
					MM_SendAuth();
				}
				break;

			case STEAMTICKET_FAILED:
				MM_Fail( "Steam would not issue a ticket: %s", CL_Steam_Error() );
				break;

			default:
				break;	// still pending
		}
		return;
	}

	if ( mm.request ) {
		reqState = CL_HTTP_RequestState( mm.request );

		if ( reqState == HTTPREQ_RUNNING ) {
			return;
		}
		if ( reqState == HTTPREQ_FAILED ) {
			char err[HTTP_MAX_ERROR];

			Q_strncpyz( err, CL_HTTP_RequestError( mm.request ), sizeof( err ) );
			MM_Fail( "cannot reach the matchmaker: %s", err[0] ? err : "unknown error" );
			return;
		}
		if ( reqState == HTTPREQ_INVALID ) {
			mm.request = 0;
			mm.state = MM_IDLE;
			return;
		}

		MM_HandleResponse();
		return;
	}

	if ( mm.state == MM_QUEUED && cls.realtime >= mm.nextPoll ) {
		if ( MM_Begin( "GET", "/v1/queue", NULL, qtrue ) ) {
			mm.state = MM_POLLING;
		}
	}

	/*
	The connect landed, so the assignment has done its job and MM_MATCHED is
	over. Retiring it here rather than leaving it set for the length of the
	match is what makes the postgame work: the state used to survive until the
	server dropped everyone at MATCH_END, at which point clc.state went back to
	CA_DISCONNECTED with mm.matchedAt minutes in the past -- so the timeout
	below fired instantly and put "could not reach the server we were given" on
	the home screen after every match that finished normally.

	Silent, because there is nothing to report: from the player's side the
	search ended when the map appeared.
	*/
	if ( mm.state == MM_MATCHED && clc.state == CA_ACTIVE ) {
		mm.state = MM_IDLE;
		MM_SetStatus( "idle", "not searching" );
	}

	/*
	Being matched is not a place to live.

	MM_MATCHED means "we have an address and the connect is under way", and it
	normally lasts about a second. Nothing used to end it, so when the connect
	did not complete -- a server refusing the ticket, an address that does not
	answer -- the client sat in it forever, showing a spinner. The menu now
	always offers a way out, but a screen that unsticks itself is better than
	one the player has to rescue.

	Only when we are not actually connected: mid-connect this state is doing its
	job, and the branch above has already retired it once the connect completed.
	*/
	if ( mm.state == MM_MATCHED
	     && clc.state == CA_DISCONNECTED
	     && cls.realtime - mm.matchedAt > MM_CONNECT_TIMEOUT ) {
		MM_Fail( "could not reach the server we were given" );
	}
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

static void CL_MM_Find_f( void ) {
	if ( mm.state != MM_IDLE && mm.state != MM_MATCHED ) {
		Com_Printf( "matchmaking: already searching -- mm_cancel first\n" );
		return;
	}
	if ( !CL_HTTP_Available() ) {
		Com_Printf( S_COLOR_YELLOW "matchmaking: HTTP support is unavailable\n" );
		return;
	}

	mm.searchStart = cls.realtime;
	Com_Printf( "matchmaking: searching via %s\n", mm_server->string );

	if ( !mm.token[0] ) {
		mm.queueAfterAuth = qtrue;
		MM_StartAuth();
		return;
	}
	MM_StartJoin();
}

static void CL_MM_Cancel_f( void ) {
	if ( mm.state == MM_IDLE ) {
		Com_Printf( "matchmaking: not searching\n" );
		return;
	}

	// Abandon whatever is in flight. Freeing a running request does not block,
	// which is the point: cancelling against a dead matchmaker must not hang.
	if ( mm.request ) {
		CL_HTTP_FreeRequest( mm.request );
		mm.request = 0;
	}
	mm.queueAfterAuth = qfalse;

	if ( !mm.token[0] ) {
		mm.state = MM_IDLE;
		MM_SetStatus( "idle", "not searching" );
		Com_Printf( "matchmaking: search cancelled\n" );
		return;
	}

	if ( MM_Begin( "DELETE", "/v1/queue", NULL, qtrue ) ) {
		mm.state = MM_CANCELLING;
		MM_SetStatus( "idle", "cancelling" );
	}
}

/*
================
CL_MM_Name_f

Sets the account's display name -- the one other players see.

Deliberately NOT the `name` cvar. That one is local and, on a matchmade server,
no longer has any effect: the server takes the name from the signed match
ticket. Setting it here means it is stored against the account, travels with
every ticket cfmm mints afterwards, and cannot be edited by whoever is holding
the ticket.
================
*/
static void CL_MM_Name_f( void ) {
	char name[MAX_NAME_LENGTH];

	if ( Cmd_Argc() < 2 ) {
		Com_Printf( "usage: mm_name <name>\n" );
		if ( mm.displayName[0] ) {
			Com_Printf( "you are currently %s\n", mm.displayName );
		}
		return;
	}

	// ArgsFrom rather than Argv, so a name with spaces in it survives.
	Q_strncpyz( name, Cmd_ArgsFrom( 1 ), sizeof( name ) );

	// Trim here so that "mm_name    " is refused with a sentence, rather than
	// travelling to cfmm and coming back as a 400.
	{
		char *s = name;
		char *end;

		while ( *s == ' ' ) {
			s++;
		}
		memmove( name, s, strlen( s ) + 1 );

		end = name + strlen( name );
		while ( end > name && end[-1] == ' ' ) {
			*--end = '\0';
		}
	}

	if ( !name[0] ) {
		Com_Printf( "matchmaking: a name cannot be empty\n" );
		return;
	}
	if ( mm.state != MM_IDLE && mm.state != MM_MATCHED ) {
		Com_Printf( "matchmaking: busy -- try again in a moment\n" );
		return;
	}
	if ( !CL_HTTP_Available() ) {
		Com_Printf( S_COLOR_YELLOW "matchmaking: HTTP support is unavailable\n" );
		return;
	}

	Q_strncpyz( mm.pendingName, name, sizeof( mm.pendingName ) );

	// Naming needs an account to name, so an unidentified client authenticates
	// first and continues in the MM_AUTH handler -- the same shape mm_find uses.
	if ( !mm.token[0] ) {
		mm.nameAfterAuth = qtrue;
		MM_StartAuth();
		return;
	}
	MM_StartSetName();
}

/*
================
CL_MM_LinkSteam_f

Attaches this machine's Steam account to the catfight account it already has.

Run once per account, by a player who has been playing on guid identity before
Steam existed. Everyone who arrives after Steam simply logs in with it and never
needs this.
================
*/
static void CL_MM_LinkSteam_f( void ) {
	if ( !CL_Steam_Available() ) {
		Com_Printf( S_COLOR_YELLOW "matchmaking: Steam is not available -- %s\n",
					CL_Steam_Error()[0] ? CL_Steam_Error() : "no helper" );
		return;
	}
	if ( mm.state != MM_IDLE && mm.state != MM_MATCHED ) {
		Com_Printf( "matchmaking: busy -- try again in a moment\n" );
		return;
	}
	if ( !CL_HTTP_Available() ) {
		Com_Printf( S_COLOR_YELLOW "matchmaking: HTTP support is unavailable\n" );
		return;
	}

	// See the authAsGuid comment: whichever branch we take below, the account
	// being linked TO must be the guid one, never a fresh Steam account.
	mm.authAsGuid = qtrue;

	if ( !mm.token[0] ) {
		mm.linkAfterAuth = qtrue;
		MM_StartAuth();
		return;
	}

	if ( CL_Steam_RequestTicket() ) {
		mm.ticketUse = TICKET_FOR_LINK;
		mm.state = MM_TICKET;
		MM_SetStatus( "idle", "checking with Steam" );
	} else {
		MM_Fail( "could not ask the Steam helper for a ticket" );
	}
}

static void CL_MM_Status_f( void ) {
	const char *what;

	switch ( mm.state ) {
		case MM_IDLE:		what = "idle";					break;
		case MM_PROVIDER:	what = "waiting on Steam";		break;
		case MM_TICKET:		what = "waiting on Steam";		break;
		case MM_LINKING:	what = "linking Steam";			break;
		case MM_AUTH:		what = "identifying";			break;
		case MM_JOINING:	what = "joining the queue";		break;
		case MM_QUEUED:		what = "queued";				break;
		case MM_POLLING:	what = "queued (polling)";		break;
		case MM_CANCELLING:	what = "cancelling";			break;
		case MM_MATCHED:	what = "matched";				break;
		default:			what = "?";						break;
	}

	Com_Printf( "matchmaker : %s\n", mm_server->string );
	Com_Printf( "state      : %s\n", what );
	Com_Printf( "status     : %s\n", mm_statusText->string );
	Com_Printf( "identity   : %s\n",
				mm.token[0] ? ( mm.displayName[0] ? mm.displayName : "(known)" )
							: "(not yet identified)" );
	Com_Printf( "provider   : %s%s\n", mm_provider->string,
				!Q_stricmp( mm_provider->string, "auto" )
					? ( MM_WantSteam() ? " (steam)"
					  : mm.steamRefused ? " (guid -- this matchmaker has no Steam)"
					  : " (guid)" ) : "" );
	Com_Printf( "player key : %s\n", Cvar_VariableString( "cl_playerKey" ) );
	if ( !CL_Steam_Available() && CL_Steam_Error()[0] ) {
		Com_Printf( "steam      : %s\n", CL_Steam_Error() );
	}

	if ( mm.state == MM_QUEUED || mm.state == MM_POLLING ) {
		Com_Printf( "searching  : %d seconds\n",
					( cls.realtime - mm.searchStart ) / 1000 );
	}
}

void CL_MM_Init( void ) {
	Com_Memset( &mm, 0, sizeof( mm ) );

	// host[:port] only -- see MM_URL for why a scheme cannot live in a cvar.
	mm_server = Cvar_Get( "mm_server", "127.0.0.1:8080", CVAR_ARCHIVE );
	mm_https = Cvar_Get( "mm_https", "0", CVAR_ARCHIVE );
	mm_autoConnect = Cvar_Get( "mm_autoConnect", "1", CVAR_ARCHIVE );
	// auto | steam | guid. See MM_WantSteam for why a client-side fallback is
	// safe here and would not be if cfmm did not get the final say.
	mm_provider = Cvar_Get( "mm_provider", "auto", CVAR_ARCHIVE );
	mm_state = Cvar_Get( "mm_state", "idle", CVAR_ROM );
	mm_statusText = Cvar_Get( "mm_statusText", "not searching", CVAR_ROM );
	Cvar_Get( "cl_matchTicket", "", CVAR_USERINFO );

	MM_LoadToken();

	Cmd_AddCommand( "mm_find", CL_MM_Find_f );
	Cmd_AddCommand( "mm_cancel", CL_MM_Cancel_f );
	Cmd_AddCommand( "mm_status", CL_MM_Status_f );
	Cmd_AddCommand( "mm_name", CL_MM_Name_f );
	Cmd_AddCommand( "mm_linkSteam", CL_MM_LinkSteam_f );
}

void CL_MM_Shutdown( void ) {
	if ( mm.request ) {
		CL_HTTP_FreeRequest( mm.request );
		mm.request = 0;
	}
	Cmd_RemoveCommand( "mm_find" );
	Cmd_RemoveCommand( "mm_cancel" );
	Cmd_RemoveCommand( "mm_status" );
	// mm_name was already being left registered here. Both go now.
	Cmd_RemoveCommand( "mm_name" );
	Cmd_RemoveCommand( "mm_linkSteam" );
}

#else /* !USE_HTTP */

// Without HTTP there is no matchmaker. The commands still exist so that a
// player typing mm_find gets an explanation rather than "unknown command".
static void CL_MM_Unavailable_f( void ) {
	Com_Printf( S_COLOR_YELLOW "matchmaking needs HTTP support, "
				"which this build was compiled without\n" );
}

void CL_MM_Init( void ) {
	Cmd_AddCommand( "mm_find", CL_MM_Unavailable_f );
	Cmd_AddCommand( "mm_cancel", CL_MM_Unavailable_f );
	Cmd_AddCommand( "mm_status", CL_MM_Unavailable_f );
	Cmd_AddCommand( "mm_name", CL_MM_Unavailable_f );
	Cmd_AddCommand( "mm_linkSteam", CL_MM_Unavailable_f );
}

void CL_MM_Frame( void ) {}

void CL_MM_Shutdown( void ) {
	Cmd_RemoveCommand( "mm_find" );
	Cmd_RemoveCommand( "mm_cancel" );
	Cmd_RemoveCommand( "mm_status" );
	Cmd_RemoveCommand( "mm_name" );
	Cmd_RemoveCommand( "mm_linkSteam" );
}

#endif /* USE_HTTP */
