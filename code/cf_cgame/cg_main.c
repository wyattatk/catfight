/*
===========================================================================
catfight -- client game module entry points.
===========================================================================
*/

#include "cg_local.h"

cg_t      cg;
cgs_t     cgs;
centity_t cg_entities[MAX_GENTITIES];

vmCvar_t cg_fov;
vmCvar_t cg_drawCrosshair;
vmCvar_t cg_nopredict;
vmCvar_t cg_showSpeed;
vmCvar_t cg_thirdPerson;
vmCvar_t cg_thirdPersonRange;
vmCvar_t cg_drawScoreboard;
vmCvar_t cg_drawStatus;

typedef struct {
	vmCvar_t   *vmCvar;
	const char *cvarName;
	const char *defaultString;
	int         cvarFlags;
} cvarTable_t;

static cvarTable_t cvarTable[] = {
	{ &cg_fov,              "cg_fov",               "90", CVAR_ARCHIVE },
	{ &cg_drawCrosshair,    "cg_drawCrosshair",      "1", CVAR_ARCHIVE },
	{ &cg_nopredict,        "cg_nopredict",          "0", 0 },
	{ &cg_showSpeed,        "cg_showSpeed",          "1", CVAR_ARCHIVE },
	{ &cg_thirdPerson,      "cg_thirdPerson",        "0", CVAR_CHEAT },
	{ &cg_thirdPersonRange, "cg_thirdPersonRange", "120", CVAR_CHEAT },
	{ &cg_drawScoreboard,   "cg_drawScoreboard",     "1", CVAR_ARCHIVE },
	{ &cg_drawStatus,       "cg_drawStatus",         "1", CVAR_ARCHIVE }
};

static const int cvarTableSize = ARRAY_LEN( cvarTable );

/*
================
Com_Printf / Com_Error

q_shared.c is engine code compiled into every module and reports problems
through these; inside a module they have to route back out through the syscall
interface.
================
*/
void QDECL Com_Printf( const char *msg, ... ) {
	va_list argptr;
	char    text[1024];

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );

	trap_Print( text );
}

void QDECL Com_Error( int level, const char *error, ... ) {
	va_list argptr;
	char    text[1024];

	(void)level;

	va_start( argptr, error );
	Q_vsnprintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

	trap_Error( text );
}

void QDECL CG_Printf( const char *msg, ... ) {
	va_list argptr;
	char    text[1024];

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );

	trap_Print( text );
}

void QDECL CG_Error( const char *msg, ... ) {
	va_list argptr;
	char    text[1024];

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );

	trap_Error( text );
}

/*
================
CG_ConfigString
================
*/
const char *CG_ConfigString( int index ) {
	if ( index < 0 || index >= MAX_CONFIGSTRINGS ) {
		CG_Error( "CG_ConfigString: bad index %i", index );
	}
	return cgs.gameState.stringData + cgs.gameState.stringOffsets[index];
}

/*
================
CG_TeamName / CG_TeamColor

The client's own idea of what a side looks like. It is kept here rather than in
cf_shared.h because it is presentation: the server has no opinion about what
colour red is.
================
*/
const char *CG_TeamName( team_t team ) {
	switch ( team ) {
	case TEAM_RED:       return "RED";
	case TEAM_BLUE:      return "BLUE";
	case TEAM_SPECTATOR: return "SPEC";
	default:             return "-";
	}
}

const float *CG_TeamColor( team_t team ) {
	static const float red[4]  = { 1.00f, 0.35f, 0.30f, 1.0f };
	static const float blue[4] = { 0.40f, 0.75f, 1.00f, 1.0f };
	static const float grey[4] = { 0.75f, 0.75f, 0.75f, 1.0f };

	switch ( team ) {
	case TEAM_RED:  return red;
	case TEAM_BLUE: return blue;
	default:        return grey;
	}
}

/*
================
CG_ParseMatchState

CS_MATCH_STATE is "<state> <stateTime> <endTime>".
================
*/
void CG_ParseMatchState( void ) {
	const char *s;

	s = CG_ConfigString( CS_MATCH_STATE );
	if ( !s[0] ) {
		return;
	}

	sscanf( s, "%i %i %i", (int *)&cgs.matchState, &cgs.matchStateTime, &cgs.matchStateEnd );
}

/*
================
CG_ParseScores

CS_SCORES is "<red> <blue> <roundNumber>".
================
*/
void CG_ParseScores( void ) {
	const char *s;

	s = CG_ConfigString( CS_SCORES );
	if ( !s[0] ) {
		return;
	}

	sscanf( s, "%i %i %i", &cgs.teamRounds[TEAM_RED], &cgs.teamRounds[TEAM_BLUE],
	        &cgs.roundNumber );
}

/*
================
CG_NewClientInfo

Re-read what we know about one player. Called whenever their CS_PLAYERS
configstring changes, which the server does on every score, death and team
change.
================
*/
void CG_NewClientInfo( int clientNum ) {
	clientInfo_t *ci;
	const char   *configstring;
	const char   *v;

	ci = &cgs.clientinfo[clientNum];

	configstring = CG_ConfigString( CS_PLAYERS + clientNum );
	if ( !configstring[0] ) {
		memset( ci, 0, sizeof( *ci ) );
		return;
	}

	memset( ci, 0, sizeof( *ci ) );
	ci->infoValid = qtrue;

	v = Info_ValueForKey( configstring, "n" );
	Q_strncpyz( ci->name, v, sizeof( ci->name ) );

	v = Info_ValueForKey( configstring, "t" );
	ci->team = (team_t)atoi( v );

	ci->score = atoi( Info_ValueForKey( configstring, "s" ) );
	ci->kills = atoi( Info_ValueForKey( configstring, "k" ) );
	ci->deaths = atoi( Info_ValueForKey( configstring, "d" ) );
	ci->eliminated = ( atoi( Info_ValueForKey( configstring, "e" ) ) != 0 );
}

static void CG_RegisterCvars( void ) {
	int          i;
	cvarTable_t *cv;

	for ( i = 0, cv = cvarTable; i < cvarTableSize; i++, cv++ ) {
		trap_Cvar_Register( cv->vmCvar, cv->cvarName, cv->defaultString, cv->cvarFlags );
	}
}

void CG_UpdateCvars( void ) {
	int          i;
	cvarTable_t *cv;

	for ( i = 0, cv = cvarTable; i < cvarTableSize; i++, cv++ ) {
		trap_Cvar_Update( cv->vmCvar );
	}
}

/*
=================
CG_RegisterGraphics

catfight has almost no art yet, so this is short on purpose. "ui/white" is a
flat quad we tint at draw time -- that is enough to build a crosshair and
simple bars out of without shipping any images.
=================
*/
static void CG_RegisterGraphics( void ) {
	CG_Printf( "loading %s\n", cgs.mapname );

	// the collision model, so prediction can trace against the world
	trap_CM_LoadMap( cgs.mapname );

	// the renderable world
	trap_R_LoadWorldMap( cgs.mapname );

	cgs.media.white = trap_R_RegisterShaderNoMip( "ui/white" );
	cgs.media.charset = trap_R_RegisterShaderNoMip( "gfx/2d/bigchars" );
	cgs.media.placeholder = trap_R_RegisterShader( "cf/placeholder" );

	/*
	Weapon audio, under the weapon's `id` rather than its display name -- see
	the note in cf_weapons.h. Renaming the gun must never mean moving assets.

	A missing sound is a non-fatal warning and a silent handle, so the game
	still runs with none of these present.
	*/
	{
		const char *id = CF_Weapon( WP_G17 )->id;

		cgs.media.fire         = trap_S_RegisterSound( va( "sound/weapons/%s/fire.wav", id ), qfalse );
		cgs.media.dryFire      = trap_S_RegisterSound( va( "sound/weapons/%s/dryfire.wav", id ), qfalse );
		cgs.media.magOut       = trap_S_RegisterSound( va( "sound/weapons/%s/magout.wav", id ), qfalse );
		cgs.media.magIn        = trap_S_RegisterSound( va( "sound/weapons/%s/magin.wav", id ), qfalse );
		cgs.media.slideRelease = trap_S_RegisterSound( va( "sound/weapons/%s/slide.wav", id ), qfalse );
	}

	cgs.media.impactFlesh = trap_S_RegisterSound( "sound/weapons/impact_flesh.wav", qfalse );
	cgs.media.impactWall  = trap_S_RegisterSound( "sound/weapons/impact_wall.wav", qfalse );
}

/*
=================
CG_ParseServerinfo
=================
*/
static void CG_ParseServerinfo( void ) {
	const char *info;
	const char *mapname;

	info = CG_ConfigString( CS_SERVERINFO );

	mapname = Info_ValueForKey( info, "mapname" );
	Com_sprintf( cgs.mapname, sizeof( cgs.mapname ), "maps/%s.bsp", mapname );

	cgs.levelStartTime = atoi( CG_ConfigString( CS_LEVEL_START_TIME ) );
}

/*
=================
CG_ParseGameState

Read everything the server already knows and we have just been handed. This is
what makes joining a match in progress work: the gamestate arrives complete, so
there is nothing to wait for.
=================
*/
static void CG_ParseGameState( void ) {
	int i;

	CG_ParseMatchState();
	CG_ParseScores();

	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		CG_NewClientInfo( i );
	}
}

/*
=================
CG_Init

Called after every level load and after every vid_restart.
=================
*/
static void CG_Init( int serverMessageNum, int serverCommandSequence, int clientNum ) {
	// clear everything, but leave the entity array alone until we know the
	// clientNum -- clearing it is cheap and avoids stale state after a restart
	memset( &cg, 0, sizeof( cg ) );
	memset( &cgs, 0, sizeof( cgs ) );
	memset( cg_entities, 0, sizeof( cg_entities ) );

	cg.clientNum = clientNum;
	cgs.processedSnapshotNum = serverMessageNum;
	cgs.serverCommandSequence = serverCommandSequence;

	CG_RegisterCvars();

	trap_GetGameState( &cgs.gameState );
	trap_GetGlconfig( &cgs.glconfig );

	cgs.screenXScale = cgs.glconfig.vidWidth / 640.0f;
	cgs.screenYScale = cgs.glconfig.vidHeight / 480.0f;

	CG_ParseServerinfo();
	CG_ParseGameState();

	CG_RegisterGraphics();

	/*
	A match starting is what retires the last one's result. The home screen
	shows it on a card between matches, and a card left standing over a game
	that has already begun would be describing a match nobody is in any more.
	*/
	trap_Cvar_Set( "cf_lastResult", "" );
	trap_Cvar_Set( "cf_lastScore", "" );
	trap_Cvar_Set( "cf_playAgain", "0" );

	// tell the engine which weapon we are holding and how much to scale mouse
	// input; nothing uses either yet, but the engine expects to be told
	trap_SetUserCmdValue( 0, 1.0f );

	CG_Printf( "catfight cgame initialised for client %i\n", clientNum );
}

static void CG_Shutdown( void ) {
	CG_Printf( "catfight cgame shutdown\n" );
}

/*
=================
CG_ConsoleCommand
=================
*/
static qboolean CG_ConsoleCommand( void ) {
	char cmd[MAX_TOKEN_CHARS];

	trap_Argv( 0, cmd, sizeof( cmd ) );

	if ( !Q_stricmp( cmd, "cf_where" ) ) {
		CG_Printf( "%.1f %.1f %.1f  yaw %.1f\n",
		           cg.predictedPlayerState.origin[0],
		           cg.predictedPlayerState.origin[1],
		           cg.predictedPlayerState.origin[2],
		           cg.predictedPlayerState.viewangles[YAW] );
		return qtrue;
	}

	// the scoreboard is a client-side overlay over information the client
	// already has, so holding it open never asks the server anything
	if ( !Q_stricmp( cmd, "+scores" ) ) {
		cg.showScores = qtrue;
		return qtrue;
	}
	if ( !Q_stricmp( cmd, "-scores" ) ) {
		cg.showScores = qfalse;
		return qtrue;
	}

	return qfalse;
}

static void CG_InitConsoleCommands( void ) {
	trap_AddCommand( "cf_where" );
	trap_AddCommand( "+scores" );
	trap_AddCommand( "-scores" );

	// commands the client does not handle itself get forwarded to the server,
	// so they have to be registered here to be typeable at all
	trap_AddCommand( "kill" );
	trap_AddCommand( "noclip" );
	trap_AddCommand( "where" );
	trap_AddCommand( "say" );
	trap_AddCommand( "team" );
}

/*
================
vmMain
================
*/
Q_EXPORT intptr_t vmMain( int command, int arg0, int arg1, int arg2, int arg3, int arg4,
                          int arg5, int arg6, int arg7, int arg8, int arg9,
                          int arg10, int arg11 ) {
	switch ( command ) {
	case CG_INIT:
		CG_Init( arg0, arg1, arg2 );
		CG_InitConsoleCommands();
		return 0;

	case CG_SHUTDOWN:
		CG_Shutdown();
		return 0;

	case CG_CONSOLE_COMMAND:
		return CG_ConsoleCommand();

	case CG_DRAW_ACTIVE_FRAME:
		CG_DrawActiveFrame( arg0, arg1, arg2 );
		return 0;

	case CG_CROSSHAIR_PLAYER:
		return -1;

	case CG_LAST_ATTACKER:
		return -1;

	case CG_KEY_EVENT:
		if ( arg1 ) {
			CG_PostgameKey( arg0 );
		}
		return 0;

	case CG_MOUSE_EVENT:
		return 0;

	case CG_EVENT_HANDLING:
		CG_PostgameEventHandling( arg0 );
		return 0;

	default:
		CG_Error( "vmMain: unknown command %i", command );
		break;
	}

	return -1;
}
