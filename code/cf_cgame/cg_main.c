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
vmCvar_t cg_forceBody;
vmCvar_t cg_animStride;
vmCvar_t cg_thirdPerson;
vmCvar_t cg_thirdPersonRange;
vmCvar_t cg_drawGun;
vmCvar_t cg_gunX;
vmCvar_t cg_gunY;
vmCvar_t cg_gunZ;
vmCvar_t cg_gunScale;
vmCvar_t cg_gunWorldScale;
vmCvar_t cg_gunHoldForward;
vmCvar_t cg_gunHoldRight;
vmCvar_t cg_gunHoldUp;
vmCvar_t cg_subtitles;
vmCvar_t cg_drawScoreboard;
vmCvar_t cg_drawStatus;
vmCvar_t cg_debugEvents;

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
	/*
	Draw EVERY player as this body, by name (models/players/<name>/), instead
	of the one the server chose for them. A testing aid: empty, the normal
	case, means each player is drawn with their own.

	NOT ARCHIVED, deliberately, and not the cvar it replaced. cg_playerModel
	was a full model path, archived, and it was once the only way to get bodies
	at all -- so it is sitting in real configs holding a path that no longer
	exists. Reusing it would have quietly forced that stale value onto every
	player forever; a new name that forgets itself on restart cannot.
	*/
	{ &cg_forceBody,        "cg_forceBody",           "", 0 },
	/*
	World units a body travels in one full run cycle, which sets how fast its
	legs turn over for a given speed. Tuned by eye, not derived: catfight moves
	far faster than a person runs, so feet that truly matched the ground would
	be a blur. Too low and they skate backwards; too high and they skate
	forwards. See CG_PlayerBody.
	*/
	{ &cg_animStride,       "cg_animStride",       "128", CVAR_ARCHIVE },
	{ &cg_thirdPerson,      "cg_thirdPerson",        "0", CVAR_CHEAT },
	{ &cg_thirdPersonRange, "cg_thirdPersonRange", "120", CVAR_CHEAT },
	/*
	The view model, and four numbers that exist to be argued with.

	Not CVAR_CHEAT, even though moving your own gun around is exactly the shape
	of thing that usually is: where a view model sits is a comfort setting, it
	is visible only to the person changing it, and no placement of it reveals
	anything -- the gun is drawn over the world, not through it. Hiding it
	entirely with cg_drawGun 0 is a preference plenty of competitive players
	have, and there is no version of this that is an advantage worth policing.

	X is forward from the eye, Y is right, Z is up, all in world units before
	the model's own scale. cg_gunScale is units per metre; see cg_weapon.c on
	why that number is tuned by eye rather than derived.
	*/
	{ &cg_drawGun,          "cg_drawGun",            "1", CVAR_ARCHIVE },
	{ &cg_gunX,             "cg_gunX",              "13", CVAR_ARCHIVE },
	{ &cg_gunY,             "cg_gunY",             "5.5", CVAR_ARCHIVE },
	{ &cg_gunZ,             "cg_gunZ",            "-6.8", CVAR_ARCHIVE },
	{ &cg_gunScale,         "cg_gunScale",          "38", CVAR_ARCHIVE },
	/*
	The weapon in OTHER players' hands, scaled against the body rather than
	against the screen. cg_gunScale above is tuned by eye for the corner of
	your own view and is deliberately not physical; sharing it would resize
	everybody else's gun whenever somebody adjusted their own. 56 units to
	roughly 1.8m is about 31 units per metre.
	*/
	{ &cg_gunWorldScale,    "cg_gunWorldScale",     "31", CVAR_ARCHIVE },
	// Where another player's weapon sits relative to their body when the body
	// has no Weapon socket to put it in -- the sprite, and static models like
	// the cube. See CG_PlayerWeapon.
	{ &cg_gunHoldForward,   "cg_gunHoldForward",    "20", CVAR_ARCHIVE },
	{ &cg_gunHoldRight,     "cg_gunHoldRight",      "10", CVAR_ARCHIVE },
	{ &cg_gunHoldUp,        "cg_gunHoldUp",          "6", CVAR_ARCHIVE },
	/*
	Her dialogue as on-screen text.

	DEFAULT ON, which is the whole point. An accessibility feature that has to
	be found in a menu is off for everyone who needed it and did not know to
	look, and the cost of it being on for a player who can hear is a line of
	text they were already being told by the console.
	*/
	{ &cg_subtitles,        "cg_subtitles",          "1", CVAR_ARCHIVE },
	{ &cg_drawScoreboard,   "cg_drawScoreboard",     "1", CVAR_ARCHIVE },
	{ &cg_drawStatus,       "cg_drawStatus",         "1", CVAR_ARCHIVE },
	// Print every entity event this client acts on. Not archived: it is a thing
	// you turn on to answer one question, not a setting.
	{ &cg_debugEvents,      "cg_debugEvents",        "0", 0 }
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

CS_SCORES is "<red> <blue> <roundNumber> <roundLimit> <maxRounds>
<lastRoundWinner>".

roundLimit, maxRounds and lastRoundWinner were appended after the fact, so each
is reset before the parse and left at its "not said" value when absent -- an
older server simply means nobody knows what the score is out of, how many
rounds are left, or who took the last one, all of which are survivable where
they are used. Every reader checks for the sentinel rather than assuming a
number arrived.

The two sentinels differ, and they have to. Zero is not a legal round limit or
cap, so it can mean "absent" for those; zero IS a legal lastRoundWinner --
TEAM_FREE, a drawn round -- so its absent value is -1.
================
*/
void CG_ParseScores( void ) {
	const char *s;
	char        dev[8];

	s = CG_ConfigString( CS_SCORES );
	if ( !s[0] ) {
		return;
	}

	cgs.roundLimit = 0;
	cgs.maxRounds = 0;
	cgs.lastRoundWinner = -1;
	sscanf( s, "%i %i %i %i %i %i", &cgs.teamRounds[TEAM_RED], &cgs.teamRounds[TEAM_BLUE],
	        &cgs.roundNumber, &cgs.roundLimit, &cgs.maxRounds, &cgs.lastRoundWinner );

	/*
	Logged at developer level so who took a round is observable from outside.

	The round banner is drawn and never printed, so the only way to find out it
	had announced ROUND LOST to the side that just won was to be looking at the
	screen when it happened -- which is how it survived as long as it did. A
	fact that only exists as pixels cannot be tested; this is the same argument
	CL_Voice_State makes for logging the situation whether or not she is
	installed. netplay/test-voicestate.ps1 reads this line.

	Gated on `developer` rather than printed outright: this fires on every
	score change, and the console belongs to the player.
	*/
	trap_Cvar_VariableStringBuffer( "developer", dev, sizeof( dev ) );
	if ( atoi( dev ) ) {
		CG_Printf( "scores: %i-%i round %i, last round to %i\n",
		           cgs.teamRounds[TEAM_RED], cgs.teamRounds[TEAM_BLUE],
		           cgs.roundNumber, cgs.lastRoundWinner );
	}
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
	char          oldBody[sizeof( ci->body )];

	ci = &cgs.clientinfo[clientNum];
	Q_strncpyz( oldBody, ci->infoValid ? ci->body : "", sizeof( oldBody ) );

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

	// Loaded now rather than on the first frame they are drawn, so a player
	// joining costs its hitch here and not in the middle of a firefight.
	Q_strncpyz( ci->body, Info_ValueForKey( configstring, "m" ), sizeof( ci->body ) );
	CG_RegisterBody( ci->body );

	// One line when somebody's body is first known or changes -- a bot moving
	// from companion to stand-in, say -- and silence the rest of the time, since
	// this runs on every score and death.
	if ( strcmp( oldBody, ci->body ) ) {
		Com_Printf( "%s is drawn as '%s'\n", ci->name, ci->body[0] ? ci->body : "(none)" );
	}
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
	cgs.media.flashModel = trap_R_RegisterModel( "models/weapons/flash.iqm" );

	/*
	The view model, under the weapon's `id` for the same reason the sounds
	below are: renaming the gun must never mean moving assets. A missing model
	returns a zero handle and CG_AddViewWeapon draws nothing, so the game runs
	without it -- but unlike a missing sound this one is worth saying out loud,
	because a silently absent gun looks like the view-model code is broken
	rather than like a file that is not there.
	*/
	{
		const char *id = CF_Weapon( WP_G17 )->id;

		cgs.media.weaponModel = trap_R_RegisterModel( va( "models/weapons/%s.iqm", id ) );
		if ( !cgs.media.weaponModel ) {
			CG_Printf( "^3no view model at models/weapons/%s.iqm -- drawing no gun\n", id );
		}
	}

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

	/*
	The world, generated by mapping/gen_world_sounds.ps1.

	Not under a weapon id: these belong to the player rather than to whatever
	they are holding. When surfaces eventually matter the path gains a material
	and this becomes a table, which is why it is a loop over an array rather
	than four named handles.
	*/
	{
		int i;

		for ( i = 0; i < 4; i++ ) {
			cgs.media.footsteps[i] =
				trap_S_RegisterSound( va( "sound/world/step%i.wav", i + 1 ), qfalse );
		}
		cgs.media.jump       = trap_S_RegisterSound( "sound/world/jump.wav", qfalse );
		cgs.media.landSoft   = trap_S_RegisterSound( "sound/world/land_soft.wav", qfalse );
		cgs.media.landMedium = trap_S_RegisterSound( "sound/world/land_medium.wav", qfalse );
		cgs.media.landHard   = trap_S_RegisterSound( "sound/world/land_hard.wav", qfalse );
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

	// Nobody has won a round yet, and the memset above spelled that as
	// TEAM_FREE -- a drawn round, which is a different and wrong claim. Set
	// before CG_ParseGameState, which may find CS_SCORES empty and leave it.
	cgs.lastRoundWinner = -1;

	CG_RegisterCvars();

	trap_GetGameState( &cgs.gameState );
	trap_GetGlconfig( &cgs.glconfig );

	cgs.screenXScale = cgs.glconfig.vidWidth / 640.0f;
	cgs.screenYScale = cgs.glconfig.vidHeight / 480.0f;

	CG_ParseServerinfo();
	CG_ClearBodies();   // handles from before a vid_restart are not handles now
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

	/*
	`cf_subtitle <text>` -- put a line on screen as though she had said it.

	Same reason cf_model exists: the thing being tested is cheap and the thing
	standing in front of it is not. Seeing a real subtitle needs the companion,
	which means loading several gigabytes of weights and then talking to her,
	and none of that exercises the part most likely to be wrong -- the wrapping,
	the placement against the rest of the HUD, and how long a line of a given
	length stays up.

	Safe to leave in. The poll in CG_DrawSubtitle only overwrites when the
	ENGINE's counter moves, so a line stuffed here survives until she actually
	speaks, and then loses to her, which is the correct precedence.
	*/
	if ( !Q_stricmp( cmd, "cf_subtitle" ) ) {
		char text[MAX_SUBTITLE_TEXT];

		trap_Args( text, sizeof( text ) );
		if ( !text[0] ) {
			CG_Printf( "usage: cf_subtitle <text>\n" );
			return qtrue;
		}
		Q_strncpyz( cg.subtitle, text, sizeof( cg.subtitle ) );
		cg.subtitleTime = cg.time;
		return qtrue;
	}

	if ( !Q_stricmp( cmd, "cf_where" ) ) {
		CG_Printf( "%.1f %.1f %.1f  yaw %.1f\n",
		           cg.predictedPlayerState.origin[0],
		           cg.predictedPlayerState.origin[1],
		           cg.predictedPlayerState.origin[2],
		           cg.predictedPlayerState.viewangles[YAW] );
		return qtrue;
	}

	/*
	`cf_model <name>` -- does the renderer accept this model file.

	The one question standing between catfight and having bodies. Every player
	is an RT_SPRITE, the way out is IQM because tr_model_iqm.c is already
	compiled into the renderer that ships, and until this command existed
	nobody had ever handed it a file to find out.

	Asked as its own command rather than inferred from something drawing,
	because the two failures look identical on screen: a model that failed to
	load and a model that loaded and is not being drawn are both nothing. The
	handle separates them in one line.

	Registration is also where a bad file is REJECTED rather than crashing --
	R_LoadIQM validates the header, the version, the declared filesize and the
	required vertex arrays, and returns 0 for any of them. A zero here means the
	file is wrong; the console carries the reason.
	*/
	if ( !Q_stricmp( cmd, "cf_model" ) ) {
		char      name[MAX_QPATH];
		qhandle_t h;

		trap_Argv( 1, name, sizeof( name ) );
		if ( !name[0] ) {
			CG_Printf( "usage: cf_model <path>   e.g. cf_model models/cube.iqm\n" );
			return qtrue;
		}

		h = trap_R_RegisterModel( name );
		if ( h ) {
			CG_Printf( "cf_model: %s LOADED, handle %i\n", name, h );
		} else {
			CG_Printf( "cf_model: %s FAILED -- see the warning above for why\n", name );
		}
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
	trap_AddCommand( "cf_model" );
	trap_AddCommand( "cf_subtitle" );
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




