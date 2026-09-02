/*
===========================================================================
catfight -- the round status bar and the scoreboard.

Both are drawn from configstrings the client already holds, never from anything
it has to ask for. That is what makes them correct for a player who connected
thirty seconds ago: there is no "current" state to have missed, only the state,
which arrives complete with the gamestate.
===========================================================================
*/

#include "cg_local.h"

static const float sbWhite[4]  = { 1.00f, 1.00f, 1.00f, 1.00f };
static const float sbDim[4]    = { 1.00f, 1.00f, 1.00f, 0.35f };
static const float sbAccent[4] = { 1.00f, 0.75f, 0.35f, 0.90f };
static const float sbShade[4]  = { 0.00f, 0.00f, 0.00f, 0.65f };
static const float sbPanel[4]  = { 0.05f, 0.06f, 0.08f, 0.88f };

/*
================
CG_MatchTimeLeft

Seconds remaining in the current match state, rounded up so that the last
second is shown as "1" rather than "0". Returns -1 when the state has no timer.
================
*/
static int CG_MatchTimeLeft( void ) {
	int msec;

	if ( !cgs.matchStateEnd ) {
		return -1;
	}

	msec = cgs.matchStateEnd - cg.time;
	if ( msec < 0 ) {
		msec = 0;
	}

	return ( msec + 999 ) / 1000;
}

/*
================
CG_RoundBanner

The big line across the middle of the screen: what is happening to the round
right now. Only shown when there is something to say -- during a live round it
is deliberately silent, because that is when the player should be looking at the
map rather than at the HUD.
================
*/
static void CG_RoundBanner( void ) {
	const char  *text;
	const float *color;
	int          seconds;
	team_t       myTeam;

	text = NULL;
	color = sbAccent;

	myTeam = (team_t)cg.predictedPlayerState.persistant[PERS_TEAM];

	switch ( cgs.matchState ) {

	case MS_WARMUP:
		text = "WARMUP -- WAITING FOR AN OPPONENT";
		color = sbDim;
		break;

	case MS_COUNTDOWN:
		seconds = CG_MatchTimeLeft();
		if ( seconds > 0 ) {
			text = va( "%i", seconds );
		} else {
			text = "FIGHT";
		}
		break;

	case MS_LIVE:
		// nothing to say; the round is the thing being said
		if ( cg.predictedPlayerState.persistant[PERS_ELIMINATED] ) {
			text = "ELIMINATED";
			color = sbDim;
		}
		break;

	case MS_ROUND_END:
		if ( cgs.teamRounds[TEAM_RED] + cgs.teamRounds[TEAM_BLUE] == 0 ) {
			text = "DRAW";
			color = sbDim;
			break;
		}
		// which side took it is not carried directly, so it is read off the
		// player's own result: you either survived the round or you did not
		if ( myTeam == TEAM_RED || myTeam == TEAM_BLUE ) {
			if ( !cg.predictedPlayerState.persistant[PERS_ELIMINATED] ) {
				text = "ROUND WON";
				color = CG_TeamColor( myTeam );
			} else {
				text = "ROUND LOST";
				color = sbDim;
			}
		} else {
			text = "ROUND OVER";
			color = sbDim;
		}
		break;

	case MS_PAUSED:
		// say how long the match is being held, so waiting is a known quantity
		// rather than an unexplained freeze
		seconds = CG_MatchTimeLeft();
		if ( seconds > 0 ) {
			text = va( "OPPONENT DISCONNECTED -- HOLDING %i", seconds );
		} else {
			text = "OPPONENT DISCONNECTED";
		}
		color = sbDim;
		break;

	case MS_MATCH_END:
		if ( cgs.teamRounds[TEAM_RED] > cgs.teamRounds[TEAM_BLUE] ) {
			text = "RED WINS THE MATCH";
			color = CG_TeamColor( TEAM_RED );
		} else if ( cgs.teamRounds[TEAM_BLUE] > cgs.teamRounds[TEAM_RED] ) {
			text = "BLUE WINS THE MATCH";
			color = CG_TeamColor( TEAM_BLUE );
		} else {
			text = "MATCH DRAWN";
			color = sbDim;
		}
		break;
	}

	if ( !text ) {
		return;
	}

	CG_DrawStringCentred( 320, 150, text, 12, 24, color );
}

/*
================
CG_DrawRoundStatus

The top-of-screen strip: the score, the round number, and the clock.
================
*/
void CG_DrawRoundStatus( void ) {
	int          seconds;
	const float *redColor;
	const float *blueColor;

	if ( !cg_drawStatus.integer ) {
		return;
	}

	redColor = CG_TeamColor( TEAM_RED );
	blueColor = CG_TeamColor( TEAM_BLUE );

	CG_FillRect( 240, 0, 160, 26, sbShade );

	// the score, with each side's own colour so it reads at a glance
	CG_DrawString( 252, 5, va( "%i", cgs.teamRounds[TEAM_RED] ), 16, 20, redColor );
	CG_DrawStringCentred( 320, 7, ":", 12, 16, sbDim );
	CG_DrawString( 372, 5, va( "%i", cgs.teamRounds[TEAM_BLUE] ), 16, 20, blueColor );

	// the clock, when the state has one
	seconds = CG_MatchTimeLeft();
	if ( seconds >= 0 && cgs.matchState == MS_LIVE ) {
		CG_DrawStringCentred( 320, 30, va( "%i:%02i", seconds / 60, seconds % 60 ),
		                      8, 14, seconds <= 10 ? sbAccent : sbDim );
	}

	if ( cgs.roundNumber > 0 && cgs.matchState != MS_WARMUP ) {
		CG_DrawStringCentred( 320, 44, va( "ROUND %i", cgs.roundNumber ), 6, 10, sbDim );
	}

	CG_RoundBanner();
}

/*
================
CG_DrawTeamColumn

One side's players. Returns the y below the column.
================
*/
static float CG_DrawTeamColumn( float x, float y, float width, team_t team ) {
	int           i;
	clientInfo_t *ci;
	const float  *color;
	int           rows;

	color = CG_TeamColor( team );

	CG_FillRect( x, y, width, 20, sbShade );
	CG_DrawString( x + 8, y + 4, CG_TeamName( team ), 10, 14, color );
	CG_DrawString( x + width - 40, y + 4, va( "%i", cgs.teamRounds[team] ), 12, 14, color );

	y += 24;

	// column headings, so the three numbers are not a guessing game
	CG_DrawString( x + width - 132, y, "RND", 6, 10, sbDim );
	CG_DrawString( x + width - 92,  y, "K",   6, 10, sbDim );
	CG_DrawString( x + width - 52,  y, "D",   6, 10, sbDim );
	y += 14;

	rows = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		ci = &cgs.clientinfo[i];

		if ( !ci->infoValid || ci->team != team ) {
			continue;
		}

		// an eliminated player is dimmed rather than hidden -- they are still on
		// the side, they are just not in this round
		CG_DrawString( x + 8, y, ci->name, 8, 12,
		               ci->eliminated ? sbDim : sbWhite );

		CG_DrawString( x + width - 132, y, va( "%i", ci->score ), 8, 12, sbWhite );
		CG_DrawString( x + width - 92,  y, va( "%i", ci->kills ), 8, 12, sbDim );
		CG_DrawString( x + width - 52,  y, va( "%i", ci->deaths ), 8, 12, sbDim );

		y += 16;
		rows++;
	}

	if ( rows == 0 ) {
		CG_DrawString( x + 8, y, "nobody", 8, 12, sbDim );
		y += 16;
	}

	return y;
}

/*
================
CG_DrawScoreboard

Returns true if it drew, so the caller knows to skip the rest of the HUD.
================
*/
qboolean CG_DrawScoreboard( void ) {
	float  y;
	float  redBottom, blueBottom;
	team_t team;
	int    i;
	int    spectators;

	if ( !cg.showScores ) {
		// the scoreboard comes up by itself once the match is decided, because
		// that is the one moment everybody wants to look at it
		if ( cgs.matchState != MS_MATCH_END ) {
			return qfalse;
		}
	}

	if ( !cg_drawScoreboard.integer ) {
		return qfalse;
	}

	CG_FillRect( 60, 60, 520, 300, sbPanel );

	CG_DrawStringCentred( 320, 72, "CATFIGHT", 12, 20, sbAccent );

	if ( cgs.matchState == MS_WARMUP ) {
		CG_DrawStringCentred( 320, 96, "WARMUP", 8, 12, sbDim );
	} else {
		// cf_roundLimit is CVAR_SERVERINFO, so the client already has it
		int limit = atoi( Info_ValueForKey( CG_ConfigString( CS_SERVERINFO ), "cf_roundLimit" ) );

		if ( limit > 0 ) {
			CG_DrawStringCentred( 320, 96, va( "FIRST TO %i ROUNDS", limit ), 8, 12, sbDim );
		} else {
			CG_DrawStringCentred( 320, 96, "ENDLESS ROUNDS", 8, 12, sbDim );
		}
	}

	y = 116;
	redBottom = CG_DrawTeamColumn( 76, y, 236, TEAM_RED );
	blueBottom = CG_DrawTeamColumn( 328, y, 236, TEAM_BLUE );

	y = ( redBottom > blueBottom ) ? redBottom : blueBottom;
	y += 12;

	// anyone sitting out, listed once at the bottom
	spectators = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		if ( !cgs.clientinfo[i].infoValid ) {
			continue;
		}
		team = cgs.clientinfo[i].team;
		if ( team != TEAM_SPECTATOR ) {
			continue;
		}

		if ( !spectators ) {
			CG_DrawString( 84, y, "SPECTATING", 6, 10, sbDim );
			y += 14;
		}
		CG_DrawString( 92, y, cgs.clientinfo[i].name, 8, 12, sbDim );
		y += 14;
		spectators++;
	}

	return qtrue;
}

// ---------------------------------------------------------------------------
// the postgame
// ---------------------------------------------------------------------------

/*
================
CG_RecordMatchResult

Write how the match went, from this client's own point of view, where the menu
can find it after the disconnect.

Cvars are the whole channel on purpose. The result has to outlive the thing
that knows it: the server drops everyone at MATCH_END, cgame is unloaded on the
way out, and the screen that shows the result afterwards is a different module
in a different address space with no connection to this one. Two cvars survive
that; nothing else here does.

The score is written with this player's own side first, because "2 - 1" only
means anything relative to the person reading it.
================
*/
static void CG_RecordMatchResult( void ) {
	team_t      myTeam;
	int         mine, theirs;
	const char *result;

	myTeam = (team_t)cg.predictedPlayerState.persistant[PERS_TEAM];

	if ( myTeam == TEAM_RED || myTeam == TEAM_BLUE ) {
		mine   = cgs.teamRounds[myTeam];
		theirs = cgs.teamRounds[myTeam == TEAM_RED ? TEAM_BLUE : TEAM_RED];

		if ( mine > theirs ) {
			result = "win";
		} else if ( mine < theirs ) {
			result = "loss";
		} else {
			result = "draw";
		}
	} else {
		// a spectator has no side to be on, so the score is reported the way it
		// is scored and the result is neither won nor lost
		mine   = cgs.teamRounds[TEAM_RED];
		theirs = cgs.teamRounds[TEAM_BLUE];
		result = "over";
	}

	trap_Cvar_Set( "cf_lastResult", result );
	trap_Cvar_Set( "cf_lastScore", va( "%i - %i", mine, theirs ) );

	// also in the log: the one line that says a client saw the match finish,
	// which is what netplay/test-matchend.ps1 checks for
	CG_Printf( "match over: %s %i - %i\n", result, mine, theirs );
}

/*
================
CG_UpdatePostgame

Enter and leave the postgame with the match state. Called every frame.
================
*/
void CG_UpdatePostgame( void ) {
	if ( cgs.matchState != MS_MATCH_END ) {
		/*
		The server resets to warmup before it drops anyone (see G_EndMatch), so
		this really does run -- and if the catcher were not handed back here, a
		player whose drop never arrived would be left holding a keyboard that
		did nothing.
		*/
		if ( cg.postgame ) {
			trap_Key_SetCatcher( trap_Key_GetCatcher() & ~KEYCATCH_CGAME );
			cg.postgame = qfalse;

			// A choice belongs to the postgame it was made in. Leaving it set
			// would make the NEXT one ignore both its keys -- which only
			// happens when the drop that was supposed to follow never came,
			// and is exactly when a player needs the keys to work.
			cg.postgameChosen = qfalse;
		}
		return;
	}

	if ( cg.postgame ) {
		return;
	}

	cg.postgame = qtrue;
	CG_RecordMatchResult();

	// Take the keyboard. The match is decided, so there is no input left worth
	// preserving, and a prompt that names two keys has to be what those keys do.
	trap_Key_SetCatcher( trap_Key_GetCatcher() | KEYCATCH_CGAME );
}

/*
================
CG_PostgameLeave

Both postgame choices. They differ only in what happens once the menu is up.

The queue cannot simply be appended to the disconnect. `disconnect` raises
ERR_DISCONNECT (CL_Disconnect_f), which longjmps out of the command buffer -- so
"disconnect; mm_find" silently does only the first half. The intent is left in a
cvar and finished by the home screen instead.
================
*/
static void CG_PostgameLeave( qboolean again ) {
	if ( cg.postgameChosen ) {
		return;
	}
	cg.postgameChosen = qtrue;

	trap_Cvar_Set( "cf_playAgain", again ? "1" : "0" );

	trap_Key_SetCatcher( trap_Key_GetCatcher() & ~KEYCATCH_CGAME );
	trap_SendConsoleCommand( "disconnect\n" );
}

void CG_PostgameKey( int key ) {
	if ( !cg.postgame ) {
		return;
	}

	switch ( key ) {
	case K_SPACE:
	case K_ENTER:
	case K_KP_ENTER:
		CG_PostgameLeave( qtrue );
		return;

	default:
		// everything else is ignored rather than taken as "menu": leaving is not
		// something to do by accident on a keyboard somebody is resting on
		return;
	}
}

/*
================
CG_PostgameEventHandling

Escape, arriving the long way round.

Escape never reaches CG_PostgameKey: the engine handles it before the catcher is
consulted ("escape always gets out of CGAME stuff", cl_keys.c), takes the
catcher back itself, and tells us afterwards with CGAME_EVENT_NONE. So this is
where escape is handled, and it is what makes the prompt's "ESC -- MENU" true.
================
*/
void CG_PostgameEventHandling( int event ) {
	if ( event != CGAME_EVENT_NONE || !cg.postgame ) {
		return;
	}

	CG_PostgameLeave( qfalse );
}

/*
================
CG_DrawPostgamePrompt

Under the final scoreboard: what the two keys do.

No cursor and no button, deliberately. cgame has neither, and the postgame is a
few seconds long -- two named keys is a smaller thing to build and a faster
thing to use than a pointer. The buttons live on the home screen, which already
has both.
================
*/
void CG_DrawPostgamePrompt( void ) {
	int seconds;

	if ( !cg.postgame ) {
		return;
	}

	if ( cg.postgameChosen ) {
		CG_DrawStringCentred( 320, 380, "LEAVING...", 8, 14, sbDim );
		return;
	}

	CG_DrawStringCentred( 320, 376, "SPACE -- PLAY AGAIN", 8, 14, sbAccent );
	CG_DrawStringCentred( 320, 398, "ESC -- MENU", 8, 14, sbDim );

	seconds = CG_MatchTimeLeft();
	if ( seconds >= 0 ) {
		CG_DrawStringCentred( 320, 424, va( "leaving by itself in %i", seconds ), 6, 10, sbDim );
	}
}
