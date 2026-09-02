/*
===========================================================================
catfight -- keeping track of the server snapshots we have received.

The client is always rendering slightly in the past: it holds the last snapshot
it has fully received (cg.snap) and the one after it (cg.nextSnap), and
interpolates entities between the two. The local player is the exception -- it
is predicted forward instead, which is what cg_predict.c does.
===========================================================================
*/

#include "cg_local.h"

/*
==================
CG_SetInitialSnapshot

The first usable snapshot. There is nothing to interpolate from, so everything
is placed exactly where the server says it is.
==================
*/
static void CG_SetInitialSnapshot( snapshot_t *snap ) {
	int            i;
	centity_t     *cent;
	entityState_t *state;

	cg.snap = snap;

	cg.predictedPlayerState = snap->ps;
	cg.validPPS = qfalse;

	for ( i = 0; i < snap->numEntities; i++ ) {
		state = &snap->entities[i];
		cent = &cg_entities[state->number];

		cent->currentState = *state;
		cent->interpolate = qfalse;
		cent->currentValid = qtrue;

		VectorCopy( cent->currentState.origin, cent->lerpOrigin );
		VectorCopy( cent->currentState.angles, cent->lerpAngles );
	}
}

/*
===================
CG_TransitionSnapshot

nextSnap becomes the current snapshot.
===================
*/
static void CG_TransitionSnapshot( void ) {
	int         i;
	centity_t  *cent;
	snapshot_t *oldFrame;

	if ( !cg.snap ) {
		CG_Error( "CG_TransitionSnapshot: NULL cg.snap" );
	}
	if ( !cg.nextSnap ) {
		CG_Error( "CG_TransitionSnapshot: NULL cg.nextSnap" );
	}

	oldFrame = cg.snap;

	// anything in the old frame is presumed gone until the new frame says
	// otherwise
	for ( i = 0; i < oldFrame->numEntities; i++ ) {
		cg_entities[oldFrame->entities[i].number].currentValid = qfalse;
	}

	cg.snap = cg.nextSnap;

	for ( i = 0; i < cg.snap->numEntities; i++ ) {
		cent = &cg_entities[cg.snap->entities[i].number];
		cent->currentState = cent->nextState;
		cent->currentValid = qtrue;
		cent->interpolate = qfalse;
	}

	cg.nextSnap = NULL;
}

/*
===================
CG_SetNextSnap
===================
*/
static void CG_SetNextSnap( snapshot_t *snap ) {
	int            num;
	entityState_t *es;
	centity_t     *cent;

	cg.nextSnap = snap;

	for ( num = 0; num < snap->numEntities; num++ ) {
		es = &snap->entities[num];
		cent = &cg_entities[es->number];

		cent->nextState = *es;

		// if this entity was not in the previous frame there is nothing to
		// interpolate from, so snap it into place
		if ( !cent->currentValid ) {
			cent->currentState = *es;
			cent->currentValid = qtrue;
			cent->interpolate = qfalse;
			VectorCopy( es->pos.trBase, cent->lerpOrigin );
			VectorCopy( es->apos.trBase, cent->lerpAngles );
		} else {
			cent->interpolate = qtrue;
		}
	}
}

/*
========================
CG_ReadNextSnapshot

Pull the next unprocessed snapshot out of the client system. Snapshots can be
dropped by the network, in which case we simply skip the missing numbers.
========================
*/
static snapshot_t *CG_ReadNextSnapshot( void ) {
	qboolean    r;
	snapshot_t *dest;

	while ( cgs.processedSnapshotNum < cg.latestSnapshotNum ) {
		// alternate between the two slots so that cg.snap stays valid while we
		// load the one after it
		if ( cg.snap == &cg.activeSnapshots[0] ) {
			dest = &cg.activeSnapshots[1];
		} else {
			dest = &cg.activeSnapshots[0];
		}

		cgs.processedSnapshotNum++;
		r = trap_GetSnapshot( cgs.processedSnapshotNum, dest );

		if ( r ) {
			CG_ExecuteNewServerCommands( dest->serverCommandSequence );
			return dest;
		}
		// dropped snapshot, try the next one
	}

	return NULL;
}

/*
============
CG_ProcessSnapshots

Called every rendered frame. Advances cg.snap / cg.nextSnap so that cg.time
falls between them, and works out how far between the two we are.
============
*/
void CG_ProcessSnapshots( void ) {
	snapshot_t *snap;
	int         n;

	trap_GetCurrentSnapshotNumber( &n, &cg.latestSnapshotTime );

	if ( n != cg.latestSnapshotNum ) {
		if ( n < cg.latestSnapshotNum ) {
			// this can happen after a vid_restart; treat it as a fresh start
			// rather than an error the player has to see
			cgs.processedSnapshotNum = n - 1;
		}
		cg.latestSnapshotNum = n;
	}

	// get the first usable snapshot
	while ( !cg.snap ) {
		snap = CG_ReadNextSnapshot();
		if ( !snap ) {
			return;   // we are still waiting on the server
		}
		if ( !( snap->snapFlags & SNAPFLAG_NOT_ACTIVE ) ) {
			CG_SetInitialSnapshot( snap );
		}
	}

	// advance until cg.time sits between snap and nextSnap
	for ( ;; ) {
		if ( !cg.nextSnap ) {
			snap = CG_ReadNextSnapshot();
			if ( !snap ) {
				break;
			}
			CG_SetNextSnap( snap );

			if ( cg.nextSnap->serverTime < cg.snap->serverTime ) {
				CG_Error( "CG_ProcessSnapshots: the server went backwards in time" );
			}
		}

		if ( cg.time >= cg.snap->serverTime && cg.time < cg.nextSnap->serverTime ) {
			break;
		}

		CG_TransitionSnapshot();
	}

	if ( cg.nextSnap ) {
		float delta;

		delta = (float)( cg.nextSnap->serverTime - cg.snap->serverTime );
		if ( delta == 0 ) {
			cg.frameInterpolation = 0;
		} else {
			cg.frameInterpolation = (float)( cg.time - cg.snap->serverTime ) / delta;
		}
	} else {
		cg.frameInterpolation = 0;
	}
}
