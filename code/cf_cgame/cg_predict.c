/*
===========================================================================
catfight -- client-side prediction.

The server is the authority on where you are, but its answer is always at least
one network round trip old. So the client re-runs the exact same movement code
over the same commands, starting from the last state the server confirmed, and
draws the result. When the server's answer arrives it replaces the guess.

This only works because cf_pmove.c is deterministic and both sides run it over
identical inputs. Anything that makes the client's copy of the move differ from
the server's shows up as the player rubber-banding.
===========================================================================
*/

#include "cg_local.h"

/*
================
CG_Trace

Prediction currently only collides against the world. Other players and movers
are not in the client's collision model yet, so you can predict yourself
straight through another player and then get corrected. That is fine while
catfight is single-player-in-an-empty-room and is the next thing to fix here.
================
*/
void CG_Trace( trace_t *result, const vec3_t start, const vec3_t mins, const vec3_t maxs,
               const vec3_t end, int skipNumber, int mask ) {
	trace_t t;

	trap_CM_BoxTrace( &t, start, end, mins, maxs, 0, mask );

	t.entityNum = ( t.fraction != 1.0f ) ? ENTITYNUM_WORLD : ENTITYNUM_NONE;

	*result = t;
}

int CG_PointContents( const vec3_t point, int passEntityNum ) {
	(void)passEntityNum;
	return trap_CM_PointContents( point, 0 );
}

/*
=================
CG_PredictPlayerState
=================
*/
void CG_PredictPlayerState( void ) {
	int            cmdNum, current;
	usercmd_t      oldestCmd, latestCmd;
	static pmove_t cg_pmove;

	if ( !cg.validPPS ) {
		cg.validPPS = qtrue;
		cg.predictedPlayerState = cg.snap->ps;
	}

	// demos and cg_nopredict just show what the server last said, which is the
	// honest view of what the server thinks and is useful for spotting
	// prediction bugs
	if ( cg_nopredict.integer || cg.demoPlayback ) {
		cg.predictedPlayerState = cg.snap->ps;
		return;
	}

	cg_pmove.ps = &cg.predictedPlayerState;
	cg_pmove.trace = CG_Trace;
	cg_pmove.pointcontents = CG_PointContents;

	current = trap_GetCurrentCmdNumber();

	// if the oldest command we still have is already newer than the state the
	// server confirmed, we cannot bridge the gap and would predict from a stale
	// base -- better to leave the last prediction standing for a frame
	trap_GetUserCmd( current - CMD_BACKUP + 1, &oldestCmd );
	if ( oldestCmd.serverTime > cg.snap->ps.commandTime && oldestCmd.serverTime < cg.time ) {
		return;
	}

	trap_GetUserCmd( current, &latestCmd );

	// start from the last authoritative state and replay everything since
	cg.predictedPlayerState = cg.snap->ps;

	for ( cmdNum = current - CMD_BACKUP + 1; cmdNum <= current; cmdNum++ ) {
		if ( !trap_GetUserCmd( cmdNum, &cg_pmove.cmd ) ) {
			continue;
		}

		// already accounted for by the server
		if ( cg_pmove.cmd.serverTime <= cg.predictedPlayerState.commandTime ) {
			continue;
		}

		// left over from before a map_restart
		if ( cg_pmove.cmd.serverTime > latestCmd.serverTime ) {
			continue;
		}

		if ( cg.predictedPlayerState.pm_type == PM_DEAD ) {
			cg_pmove.tracemask = MASK_DEADSOLID;
		} else {
			cg_pmove.tracemask = MASK_PLAYERSOLID;
		}

		CF_Pmove( &cg_pmove );
	}
}
