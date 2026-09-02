/*
===========================================================================
catfight -- reacting to events.

An event is the server (or our own predicted movement) telling us that something
*happened*. State replication cannot carry that: by the time a snapshot arrives,
a hit is over and only its consequences are in the state.

catfight has no sound and almost no art, so most of what follows is a console
line where an effect will eventually be. That is deliberate rather than lazy --
the plumbing is the part that is hard to add later, and having every event
already arrive at the right place means adding the effect is a one-line change
in one file.
===========================================================================
*/

#include "cg_local.h"

/*
=================
CG_EntityEvent

Handle one event from one entity. position is where it happened.
=================
*/
void CG_EntityEvent( centity_t *cent, const vec3_t position ) {
	entityState_t *es;
	int            event;
	int            clientNum;

	(void)position;

	es = &cent->currentState;
	event = es->event & ~EV_EVENT_BITS;

	if ( !event ) {
		return;
	}

	clientNum = es->clientNum;
	if ( clientNum < 0 || clientNum >= MAX_CLIENTS ) {
		clientNum = 0;
	}

	switch ( event ) {

	// ---------------------------------------------------------- movement --
	case EV_FOOTSTEP:
	case EV_FALL_SHORT:
	case EV_FALL_MEDIUM:
	case EV_FALL_FAR:
	case EV_JUMP:
		// these want sounds, and catfight has none yet
		break;

	// ------------------------------------------------------------ combat --
	case EV_PAIN:
		break;

	case EV_DEATH:
		break;

	// ----------------------------------------------------------- weapons --
	/*
	Positional, including our own. A first-person shot played as a local sound
	would be the only thing in the world with no direction, and on a weapon
	whose whole job is to be fired that reads as detachment from it.
	*/
	case EV_FIRE:
		trap_S_StartSound( position, es->number, CHAN_WEAPON, cgs.media.fire );
		break;

	case EV_DRY_FIRE:
		trap_S_StartSound( position, es->number, CHAN_WEAPON, cgs.media.dryFire );
		break;

	/*
	The magazine leaving the gun. The rest of the reload is scheduled from
	here rather than sent as more events, because the timings are already
	known from the weapon table and pmove is already counting them down --
	sending three events for one action would just be three chances to
	desynchronise.

	eventParm is set when the slide was locked back, which is the difference
	between the two cadences and the thing a listener can actually hear: an
	empty reload ends with the slide slamming forward, a tactical one does
	not.
	*/
	case EV_RELOAD:
		trap_S_StartSound( position, es->number, CHAN_WEAPON, cgs.media.magOut );
		cent->reloadFromEmpty = ( es->eventParm != 0 );
		break;

	case EV_RELOAD_DONE:
		trap_S_StartSound( position, es->number, CHAN_WEAPON, cgs.media.magIn );
		if ( cent->reloadFromEmpty ) {
			trap_S_StartSound( position, es->number, CHAN_AUTO, cgs.media.slideRelease );
			cent->reloadFromEmpty = qfalse;
		}
		break;

	// ----------------------------------------------------------- impacts --
	/*
	otherEntityNum2 is who fired. The hit marker is confirmation for the
	person who pulled the trigger and nobody else, so it is gated on that
	rather than shown to every client that can hear the impact.
	*/
	case EV_BULLET_FLESH:
		trap_S_StartSound( position, ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.impactFlesh );
		if ( es->otherEntityNum2 == cg.snap->ps.clientNum ) {
			cg.hitMarkerTime = cg.time;
		}
		break;

	case EV_BULLET_WALL:
		trap_S_StartSound( position, ENTITYNUM_WORLD, CHAN_AUTO, cgs.media.impactWall );
		break;

	// --------------------------------------------------------- spawning --
	case EV_PLAYER_TELEPORT_IN:
	case EV_PLAYER_TELEPORT_OUT:
		break;

	// ------------------------------------------------------------ match --
	case EV_ROUND_START:
		CG_Printf( "round %i\n", es->eventParm );
		break;

	case EV_ROUND_WON:
	case EV_ROUND_LOST:
		// the round banner is drawn from the match state instead, so that it is
		// still correct for someone who joined a moment ago
		break;

	default:
		CG_Printf( "Unknown event: %i\n", event );
		break;
	}
}

/*
==============
CG_CheckEvents

Fire any event this entity is carrying that we have not fired already.

The guard matters more than it looks. An entity's state is resent in every
snapshot it appears in, so without remembering what we last acted on, one death
would be announced twenty times a second for as long as the corpse was visible.
==============
*/
void CG_CheckEvents( centity_t *cent ) {
	// an entity that *is* an event -- G_TempEntity, whose whole existence is one
	// thing happening at one place
	if ( cent->currentState.eType > ET_EVENTS ) {
		if ( cent->previousEvent ) {
			return;
		}
		if ( cent->currentState.number == cg.snap->ps.clientNum ) {
			return;   // our own predicted events already fired locally
		}

		cent->previousEvent = 1;
		cent->currentState.event = cent->currentState.eType - ET_EVENTS;

		CG_EntityEvent( cent, cent->lerpOrigin );
		return;
	}

	// an ordinary entity carrying an event alongside its state
	if ( cent->currentState.event == cent->previousEvent ) {
		return;
	}
	cent->previousEvent = cent->currentState.event;
	if ( ( cent->currentState.event & ~EV_EVENT_BITS ) == 0 ) {
		return;
	}

	CG_EntityEvent( cent, cent->lerpOrigin );
}
