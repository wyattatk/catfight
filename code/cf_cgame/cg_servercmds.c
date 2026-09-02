/*
===========================================================================
catfight -- reliable commands sent from the server to this client.

These are text commands, delivered in order and guaranteed to arrive, used for
anything that must not be dropped: chat, configstring updates, level changes.
===========================================================================
*/

#include "cg_local.h"

static const char *CG_Argv( int arg ) {
	static char buffer[MAX_STRING_CHARS];

	trap_Argv( arg, buffer, sizeof( buffer ) );

	return buffer;
}

/*
=================
CG_ConfigStringModified

The engine has already merged the new value into its own gamestate, so the
cheapest correct thing to do is take a fresh copy.
=================
*/
static void CG_ConfigStringModified( void ) {
	int         num;
	const char *str;

	num = atoi( CG_Argv( 1 ) );

	trap_GetGameState( &cgs.gameState );

	str = CG_ConfigString( num );

	if ( num == CS_LEVEL_START_TIME ) {
		cgs.levelStartTime = atoi( str );
		return;
	}

	if ( num == CS_MATCH_STATE ) {
		CG_ParseMatchState();
		return;
	}

	if ( num == CS_SCORES ) {
		CG_ParseScores();
		return;
	}

	if ( num >= CS_PLAYERS && num < CS_PLAYERS + MAX_CLIENTS ) {
		CG_NewClientInfo( num - CS_PLAYERS );
		return;
	}
}

/*
=================
CG_ServerCommand
=================
*/
static void CG_ServerCommand( void ) {
	const char *cmd;

	cmd = CG_Argv( 0 );

	if ( !cmd[0] ) {
		return;   // the server claimed the command for itself
	}

	if ( !strcmp( cmd, "cs" ) ) {
		CG_ConfigStringModified();
		return;
	}

	if ( !strcmp( cmd, "print" ) ) {
		CG_Printf( "%s", CG_Argv( 1 ) );
		return;
	}

	if ( !strcmp( cmd, "chat" ) ) {
		CG_Printf( "%s\n", CG_Argv( 1 ) );
		return;
	}

	if ( !strcmp( cmd, "disconnect" ) ) {
		// the engine handles the actual disconnect; this is just the reason
		CG_Printf( "disconnected: %s\n", CG_Argv( 1 ) );
		return;
	}

	if ( !strcmp( cmd, "map_restart" ) ) {
		cg.validPPS = qfalse;
		return;
	}

	CG_Printf( "Unknown server command: %s\n", cmd );
}

/*
====================
CG_ExecuteNewServerCommands

Execute every server command we have not seen yet, in order.
====================
*/
void CG_ExecuteNewServerCommands( int latestSequence ) {
	while ( cgs.serverCommandSequence < latestSequence ) {
		if ( trap_GetServerCommand( ++cgs.serverCommandSequence ) ) {
			CG_ServerCommand();
		}
	}
}
