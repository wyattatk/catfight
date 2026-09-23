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
NO TRACERS, deliberately. A pistol round does not leave a visible streak, and the
beams catfight used to draw between shooter and impact looked like it. Where a
shot came from is carried by what a shot really gives away: the report, which
is positional (EV_FIRE below), and the muzzle flash lighting the wall beside
whoever fired (CG_AddMuzzleFlash).
*/

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

	/*
	Every event this client actually acts on, on request.

	Worth having permanently because an event that never arrives and an event
	that arrives and does nothing are indistinguishable from the outside -- both
	are just a missing sound or a missing hit marker. That ambiguity is exactly
	what hid the dropped-event bug above, and it is the only way to check a
	client-side effect without a person watching the screen.
	*/
	if ( cg_debugEvents.integer ) {
		// The parm goes LAST: netplay/test-ownevents.ps1 parses the
		// "event <n> from entity <num> " prefix and must keep matching it.
		CG_Printf( "event %i from entity %i (by %i) parm %i\n",
		           event, es->number, es->otherEntityNum2, es->eventParm );
	}

	switch ( event ) {

	// ---------------------------------------------------------- movement --
	/*
	FOOTSTEPS ARE INFORMATION, not decoration. They are the only way to know
	somebody is behind you, and they are what makes the walk key a decision
	instead of a strictly worse run -- see PM_Footsteps, where walking and
	crouching deliberately raise no event at all.

	Positional and on CHAN_BODY, so they mix with the gun rather than cutting
	it off: a step during a burst must not silence the burst.

	The variant is chosen from the event's own ring position rather than from a
	random number, so that everybody hearing the same step hears the same foot.
	Random here would be locally plausible and globally inconsistent, which is
	the same class of mistake as predicting a sound the server never raised.
	*/
	case EV_FOOTSTEP:
		trap_S_StartSound( position, es->number, CHAN_BODY,
		                   cgs.media.footsteps[ ( es->event >> 8 ) & 3 ] );
		break;

	case EV_JUMP:
		trap_S_StartSound( position, es->number, CHAN_BODY, cgs.media.jump );
		break;

	/*
	Three landings, differing in body rather than in volume -- "that was a long
	way down" is a LOWER sound, not a louder one. EV_FALL_FAR is the one that
	pairs with fall damage actually being taken.
	*/
	case EV_FALL_SHORT:
		trap_S_StartSound( position, es->number, CHAN_BODY, cgs.media.landSoft );
		break;

	case EV_FALL_MEDIUM:
		trap_S_StartSound( position, es->number, CHAN_BODY, cgs.media.landMedium );
		break;

	case EV_FALL_FAR:
		trap_S_StartSound( position, es->number, CHAN_BODY, cgs.media.landHard );
		break;

	// ------------------------------------------------------------ combat --
	case EV_PAIN:
		break;

	/*
	OUR OWN death starts the death camera and remembers who did it; the
	eventParm is the attacker's entity number (ENTITYNUM_WORLD for a fall).
	Everybody else's death needs nothing here -- their body falls because its
	state says EF_DEAD.
	*/
	case EV_DEATH:
		if ( cent == &cg.predictedPlayerEntity ) {
			cg.deathTime = cg.time ? cg.time : 1;
			cg.killer = es->eventParm;
		}
		break;

	// ----------------------------------------------------------- weapons --
	/*
	Positional, including our own. A first-person shot played as a local sound
	would be the only thing in the world with no direction, and on a weapon
	whose whole job is to be fired that reads as detachment from it.
	*/
	case EV_FIRE:
		trap_S_StartSound( position, es->number, CHAN_WEAPON, cgs.media.fire );
		// The flash is drawn for the next few frames, not this instant -- see
		// CG_AddMuzzleFlash. Recorded on the shooter, so it lights the room even
		// when the shooter is somebody else behind a corner.
		cent->muzzleFlashTime = cg.time;
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
		/*
		Start the clock the body and the magazine are animated along. The
		event is the only moment another client learns a reload began, and it
		says which kind, which is all it takes to know how long it lasts: the
		weapon table is compiled into both sides.
		*/
		{
			const cf_weaponInfo_t *w = CF_Weapon( es->weapon );

			cent->reloadTime = cg.time;
			cent->reloadTotal = w ? ( cent->reloadFromEmpty ? w->reloadEmpty : w->reloadTactical ) : 0;
		}
		break;

	case EV_RELOAD_DONE:
		cent->reloadTime = 0;
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
		// The body it landed in flinches away from it.
		CG_FlinchFromImpact( es->otherEntityNum2, position );
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
CG_CheckPlayerstateEvents

Our OWN events, which reach us nowhere else.

THIS IS WHY THE SHOOTER COULD NOT HEAR HIS OWN GUN. Events raised by pmove go
into ps->events, and cf_entstate.c copies them into the entity state so that
everybody ELSE hears them -- but CG_AddPacketEntities skips our own body in
first person, so the one player guaranteed to care about the shot was the only
one it never reached. No report, no dry-fire click, no reload, for the whole
life of the project. Every sound was already registered and every event was
already being raised; there was simply no path from our own playerState to
CG_EntityEvent.

The server's own events for us arrive the same way and were equally lost:
G_AddEvent on a client writes ps->externalEvent, so our own pain and death were
silent too.

FIRED BY SEQUENCE NUMBER, NOT BY VALUE, and the difference is the whole
correctness of this function. ps->events is a four-slot ring; two shots in
quick succession write the same value into different slots, so "the event
changed" would miss the second one. eventSequence only ever counts up, so
comparing it against the previous frame's answers "which of these have I not
acted on" exactly.

The second clause of the test catches a ring that wrapped between frames --
with MAX_PS_EVENTS of 4 and a bad enough frame hitch that is reachable, and
without it the wrapped entries would be silently skipped.
==============
*/
void CG_CheckPlayerstateEvents( playerState_t *ps, playerState_t *ops ) {
	centity_t *cent = &cg.predictedPlayerEntity;
	int        i, event;

	/*
	The state this pretends to be. CG_EntityEvent reads number for the sound
	channel and clientNum to attribute it, and this centity is never in
	cg_entities -- it exists so that our own events can go through exactly the
	same handler as everybody else's rather than a parallel copy of it that
	would drift.
	*/
	cent->currentState.number    = ps->clientNum;
	cent->currentState.clientNum = ps->clientNum;

	// The server forcing an event onto us -- pain, death. One at a time, and
	// only when it is not the one we already acted on.
	if ( ps->externalEvent && ps->externalEvent != ops->externalEvent ) {
		cent->currentState.event     = ps->externalEvent;
		cent->currentState.eventParm = ps->externalEventParm;
		CG_EntityEvent( cent, ps->origin );
	}

	for ( i = ps->eventSequence - MAX_PS_EVENTS; i < ps->eventSequence; i++ ) {
		if ( i < 0 ) {
			continue;
		}
		if ( i >= ops->eventSequence
		     || ( i > ops->eventSequence - MAX_PS_EVENTS
		          && ps->events[i & ( MAX_PS_EVENTS - 1 )]
		             != ops->events[i & ( MAX_PS_EVENTS - 1 )] ) ) {

			event = ps->events[i & ( MAX_PS_EVENTS - 1 )];

			/*
			Carrying the rotating sequence bits, exactly as cf_entstate.c does
			when it hands the same event to everybody else.

			CG_EntityEvent masks them off to get the event, so nothing depends
			on them being here -- except anything that uses them to vary an
			effect. The footstep variant does, and without these our own steps
			would all be the same foot while every other player's cycled
			through four.
			*/
			cent->currentState.event     = event | ( ( i & 3 ) << 8 );
			cent->currentState.eventParm = ps->eventParms[i & ( MAX_PS_EVENTS - 1 )];

			/*
			Positioned at our own origin rather than played flat, which is what
			CG_EntityEvent's own comment asks for: a first-person shot with no
			direction would be the only sound in the world without one. The
			listener sits at the eye, so this lands centred without being
			special-cased.
			*/
			CG_EntityEvent( cent, ps->origin );
		}
	}

	ops->eventSequence = ps->eventSequence;
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
	/*
	An entity that *is* an event -- G_TempEntity, whose whole existence is one
	thing happening at one place.

	previousEvent stops it being fired again on each of the several snapshots it
	survives for. It is cleared when the slot next arrives fresh (see
	CG_SetNextSnap); without that it was never cleared at all, and every slot
	fired one event ever.

	There used to be a guard here skipping the event when the entity number
	matched our own client number, on the grounds that we predict our own events.
	It was dead code: a temp entity is allocated above MAX_CLIENTS and a client
	number is below it, so the two can never be equal. Our own predicted events
	come through the playerState and never reach this path at all.
	*/
	if ( cent->currentState.eType > ET_EVENTS ) {
		if ( cent->previousEvent ) {
			return;
		}

		cent->previousEvent = 1;
		cent->currentState.event = cent->currentState.eType - ET_EVENTS;

		CG_EntityEvent( cent, cent->lerpOrigin );
		return;
	}

	/*
	An ordinary entity carrying an event alongside its state.

	OUR OWN BODY IS SKIPPED HERE, and unlike the dead guard described above
	this one is real: an ordinary entity number CAN equal our client number,
	and our events already came through the playerState. In first person we
	never get this far -- CG_AddPacketEntities does not add our own body at all
	-- but in third person it does, and without this every shot would be heard
	twice. Correctness that depends on cg_thirdPerson being off is not
	correctness.
	*/
	if ( cent->currentState.number == cg.snap->ps.clientNum ) {
		return;
	}

	if ( cent->currentState.event == cent->previousEvent ) {
		return;
	}
	cent->previousEvent = cent->currentState.event;
	if ( ( cent->currentState.event & ~EV_EVENT_BITS ) == 0 ) {
		return;
	}

	CG_EntityEvent( cent, cent->lerpOrigin );
}
