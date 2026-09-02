/*
===========================================================================
catfight -- player movement.

Run identically by the server (to decide where you actually are) and by the
client (to predict it, so the game does not feel like it is happening by post).
Both sides feed it the same usercmd_t and the same playerState_t and must get
bit-identical results out, which is why velocity is snapped to integers at the
end of every step.

The shape of the code -- accelerate toward a wish direction, then slide the
bounding box through the world clipping against the planes it hits -- is the
standard way to move a body through a BSP. The numbers in cf_shared.h, and the
decisions about what the numbers are allowed to do (see CF_AIR_CONTROL_CLAMP),
are catfight's own.
===========================================================================
*/

#include "cf_shared.h"
#include "cf_weapons.h"

typedef struct {
	vec3_t   forward, right, up;   // view axes, flattened for ground movement
	float    frametime;            // seconds this step covers
	int      msec;

	qboolean walking;              // standing on something walkable
	qboolean groundPlane;          // touching a plane, walkable or not
	trace_t  groundTrace;

	float    impactSpeed;          // downward speed at the moment of landing

	vec3_t   previous_origin;
	vec3_t   previous_velocity;
} pml_t;

static pmove_t *pm;
static pml_t    pml;

static void PM_AirMove( void );

/*
================
PM_AddEvent

Events ride out to the client in the playerState so cgame can play a sound or
throw a particle without the server having to spawn a temporary entity.
================
*/
static void PM_AddEventParm( int newEvent, int parm ) {
	if ( !newEvent ) {
		return;
	}
	pm->ps->events[pm->ps->eventSequence & (MAX_PS_EVENTS - 1)] = newEvent;
	pm->ps->eventParms[pm->ps->eventSequence & (MAX_PS_EVENTS - 1)] = parm;
	pm->ps->eventSequence++;
}

static void PM_AddEvent( int newEvent ) {
	PM_AddEventParm( newEvent, 0 );
}

static void PM_AddTouchEnt( int entityNum ) {
	int i;

	if ( entityNum == ENTITYNUM_WORLD || pm->numtouch == MAXTOUCH ) {
		return;
	}
	for ( i = 0; i < pm->numtouch; i++ ) {
		if ( pm->touchents[i] == entityNum ) {
			return;
		}
	}
	pm->touchents[pm->numtouch++] = entityNum;
}

/*
==================
PM_ClipVelocity

Remove the component of velocity heading into a plane, leaving the component
that slides along it. overbounce pushes very slightly back out of the plane so
that the next trace does not start exactly on its surface and immediately
re-collide.
==================
*/
static void PM_ClipVelocity( const vec3_t in, const vec3_t normal, vec3_t out, float overbounce ) {
	float backoff;
	int   i;

	backoff = DotProduct( in, normal );

	if ( backoff < 0 ) {
		backoff *= overbounce;
	} else {
		backoff /= overbounce;
	}

	for ( i = 0; i < 3; i++ ) {
		out[i] = in[i] - normal[i] * backoff;
	}
}

/*
==================
CF_PlayerBounds

The bounding box for a given player state. Shared so the game module can size
entities the same way pmove does.
==================
*/
void CF_PlayerBounds( const playerState_t *ps, vec3_t mins, vec3_t maxs ) {
	mins[0] = CF_MINS_X;
	mins[1] = CF_MINS_Y;
	mins[2] = CF_MINS_Z;

	maxs[0] = CF_MAXS_X;
	maxs[1] = CF_MAXS_Y;
	maxs[2] = ( ps->pm_flags & PMF_DUCKED ) ? CF_CROUCH_MAXS_Z : CF_MAXS_Z;
}

/*
==================
PM_Friction

Bleed off speed. Below CF_STOP_SPEED the drop is computed as though you were
moving at CF_STOP_SPEED, so that slow movement stops promptly instead of
decaying asymptotically and leaving you drifting.
==================
*/
static void PM_Friction( void ) {
	vec3_t vec;
	float *vel;
	float  speed, newspeed, control, drop;

	vel = pm->ps->velocity;

	VectorCopy( vel, vec );
	if ( pml.walking ) {
		vec[2] = 0;   // ignore slope movement
	}

	speed = VectorLength( vec );
	if ( speed < 1 ) {
		vel[0] = 0;
		vel[1] = 0;   // allow sinking underwater / falling
		return;
	}

	drop = 0;

	// only apply friction when actually standing on something walkable;
	// there is no air friction, and ice (SURF_SLICK) has none either
	if ( pml.walking && !( pml.groundTrace.surfaceFlags & SURF_SLICK ) &&
	     pm->ps->pm_type != PM_DEAD ) {
		control = speed < CF_STOP_SPEED ? CF_STOP_SPEED : speed;
		drop += control * CF_GROUND_FRICTION * pml.frametime;
	}

	newspeed = speed - drop;
	if ( newspeed < 0 ) {
		newspeed = 0;
	}
	newspeed /= speed;

	VectorScale( vel, newspeed, vel );
}

/*
==============
PM_Accelerate

Accelerate toward wishdir, but only up to wishspeed *along that direction*.
Speed already carried perpendicular to wishdir is left alone, which is what
makes turning while moving feel like steering rather than braking.
==============
*/
static void PM_Accelerate( const vec3_t wishdir, float wishspeed, float accel ) {
	float currentspeed, addspeed, accelspeed;

	currentspeed = DotProduct( pm->ps->velocity, wishdir );
	addspeed = wishspeed - currentspeed;
	if ( addspeed <= 0 ) {
		return;
	}

	accelspeed = accel * pml.frametime * wishspeed;
	if ( accelspeed > addspeed ) {
		accelspeed = addspeed;
	}

	VectorMA( pm->ps->velocity, accelspeed, wishdir, pm->ps->velocity );
}

/*
============
PM_CmdScale

Turn the -127..127 movement axes into a fraction of ps->speed, normalised so
that holding forward+strafe is not faster than holding forward alone.
============
*/
static float PM_CmdScale( const usercmd_t *cmd ) {
	int   max;
	float total;
	float scale;

	max = abs( cmd->forwardmove );
	if ( abs( cmd->rightmove ) > max ) {
		max = abs( cmd->rightmove );
	}
	if ( abs( cmd->upmove ) > max ) {
		max = abs( cmd->upmove );
	}
	if ( !max ) {
		return 0;
	}

	total = sqrt( (float)( cmd->forwardmove * cmd->forwardmove
	                     + cmd->rightmove * cmd->rightmove
	                     + cmd->upmove * cmd->upmove ) );
	scale = (float)pm->ps->speed * max / ( 127.0f * total );

	return scale;
}

/*
============
PM_LimitWishSpeed

Crouching and walking are speed limits rather than separate movement modes.
============
*/
static float PM_LimitWishSpeed( float wishspeed ) {
	float limit;

	if ( pm->ps->pm_flags & PMF_DUCKED ) {
		if ( wishspeed > CF_CROUCH_SPEED ) {
			wishspeed = CF_CROUCH_SPEED;
		}
	} else if ( pm->cmd.buttons & BUTTON_WALKING ) {
		limit = pm->ps->speed * CF_WALK_SPEED_SCALE;
		if ( wishspeed > limit ) {
			wishspeed = limit;
		}
	}

	return wishspeed;
}

/*
=============
PM_CheckJump
=============
*/
static qboolean PM_CheckJump( void ) {
	if ( pm->ps->pm_type == PM_DEAD ) {
		return qfalse;
	}

	if ( pm->cmd.upmove < 10 ) {
		pm->ps->pm_flags &= ~PMF_JUMP_HELD;
		return qfalse;
	}

	// a held jump key does not re-trigger; you have to let go and press again
	if ( pm->ps->pm_flags & PMF_JUMP_HELD ) {
		return qfalse;
	}

	pml.groundPlane = qfalse;
	pml.walking = qfalse;
	pm->ps->pm_flags |= PMF_JUMP_HELD;
	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pm->ps->velocity[2] = CF_JUMP_VELOCITY;

	PM_AddEvent( EV_JUMP );

	return qtrue;
}

/*
==================
PM_SlideMove

Move through the world, clipping velocity against every plane hit and
re-tracing, so that running into a wall at an angle slides along it instead of
stopping dead.

When more than one plane is in the way the two are treated as a crease and
velocity is projected along the line where they meet; if a third plane blocks
even that, the move is abandoned. Returns true if anything blocked the move.
==================
*/
#define MAX_CLIP_PLANES 5

static qboolean PM_SlideMove( qboolean gravity ) {
	int     bumpcount, numbumps;
	vec3_t  dir;
	float   d;
	int     numplanes;
	vec3_t  planes[MAX_CLIP_PLANES];
	vec3_t  clipVelocity;
	int     i, j, k;
	trace_t trace;
	vec3_t  end;
	float   time_left;
	float   into;
	vec3_t  endVelocity;
	vec3_t  endClipVelocity;

	numbumps = 4;

	if ( gravity ) {
		VectorCopy( pm->ps->velocity, endVelocity );
		endVelocity[2] -= pm->ps->gravity * pml.frametime;
		// integrate gravity over the step with the trapezoid rule, so that the
		// distance fallen does not depend on the frame rate
		pm->ps->velocity[2] = ( pm->ps->velocity[2] + endVelocity[2] ) * 0.5f;
		if ( pml.groundPlane ) {
			// slide along the ground plane
			PM_ClipVelocity( pm->ps->velocity, pml.groundTrace.plane.normal,
			                 pm->ps->velocity, CF_OVERCLIP );
		}
	} else {
		VectorCopy( pm->ps->velocity, endVelocity );
	}

	time_left = pml.frametime;

	// never turn against the ground plane
	numplanes = 0;
	if ( pml.groundPlane ) {
		VectorCopy( pml.groundTrace.plane.normal, planes[numplanes] );
		numplanes++;
	}

	// never turn against the original velocity
	VectorNormalize2( pm->ps->velocity, planes[numplanes] );
	numplanes++;

	for ( bumpcount = 0; bumpcount < numbumps; bumpcount++ ) {

		// calculate position we are trying to move to
		VectorMA( pm->ps->origin, time_left, pm->ps->velocity, end );

		// see if we can make it there
		pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, end, pm->ps->clientNum, pm->tracemask );

		if ( trace.allsolid ) {
			// entirely trapped in another solid
			pm->ps->velocity[2] = 0;   // don't build up falling damage, but allow sideways acceleration
			return qtrue;
		}

		if ( trace.fraction > 0 ) {
			// actually covered some distance
			VectorCopy( trace.endpos, pm->ps->origin );
		}

		if ( trace.fraction == 1 ) {
			break;   // moved the entire distance
		}

		PM_AddTouchEnt( trace.entityNum );

		time_left -= time_left * trace.fraction;

		if ( numplanes >= MAX_CLIP_PLANES ) {
			// this shouldn't really happen
			VectorClear( pm->ps->velocity );
			return qtrue;
		}

		//
		// if this is the same plane we hit before, nudge velocity out along it,
		// which fixes some epsilon issues with non-axial planes
		//
		for ( i = 0; i < numplanes; i++ ) {
			if ( DotProduct( trace.plane.normal, planes[i] ) > 0.99f ) {
				VectorAdd( trace.plane.normal, pm->ps->velocity, pm->ps->velocity );
				break;
			}
		}
		if ( i < numplanes ) {
			continue;
		}
		VectorCopy( trace.plane.normal, planes[numplanes] );
		numplanes++;

		//
		// modify velocity so it parallels all of the clip planes
		//

		// find a plane that it enters
		for ( i = 0; i < numplanes; i++ ) {
			into = DotProduct( pm->ps->velocity, planes[i] );
			if ( into >= 0.1f ) {
				continue;   // move doesn't interact with the plane
			}

			// see how hard we are hitting things
			if ( -into > pml.impactSpeed ) {
				pml.impactSpeed = -into;
			}

			// slide along the plane
			PM_ClipVelocity( pm->ps->velocity, planes[i], clipVelocity, CF_OVERCLIP );
			PM_ClipVelocity( endVelocity, planes[i], endClipVelocity, CF_OVERCLIP );

			// see if there is a second plane that the new move enters
			for ( j = 0; j < numplanes; j++ ) {
				if ( j == i ) {
					continue;
				}
				if ( DotProduct( clipVelocity, planes[j] ) >= 0.1f ) {
					continue;   // move doesn't interact with the plane
				}

				// try clipping the move to the plane
				PM_ClipVelocity( clipVelocity, planes[j], clipVelocity, CF_OVERCLIP );
				PM_ClipVelocity( endClipVelocity, planes[j], endClipVelocity, CF_OVERCLIP );

				// see if it goes back into the first clip plane
				if ( DotProduct( clipVelocity, planes[i] ) >= 0 ) {
					continue;
				}

				// slide the original velocity along the crease where the two
				// planes meet
				CrossProduct( planes[i], planes[j], dir );
				VectorNormalize( dir );
				d = DotProduct( dir, pm->ps->velocity );
				VectorScale( dir, d, clipVelocity );

				CrossProduct( planes[i], planes[j], dir );
				VectorNormalize( dir );
				d = DotProduct( dir, endVelocity );
				VectorScale( dir, d, endClipVelocity );

				// see if there is a third plane the new move enters
				for ( k = 0; k < numplanes; k++ ) {
					if ( k == i || k == j ) {
						continue;
					}
					if ( DotProduct( clipVelocity, planes[k] ) >= 0.1f ) {
						continue;   // move doesn't interact with the plane
					}
					// stop dead at a triple plane interaction
					VectorClear( pm->ps->velocity );
					return qtrue;
				}
			}

			// if we have fixed all interactions, try another move
			VectorCopy( clipVelocity, pm->ps->velocity );
			VectorCopy( endClipVelocity, endVelocity );
			break;
		}
	}

	if ( gravity ) {
		VectorCopy( endVelocity, pm->ps->velocity );
	}

	return ( bumpcount != 0 );
}

/*
==================
PM_StepSlideMove

A slide move that can climb steps: if the plain move was blocked, try the same
move again from CF_STEP_HEIGHT higher up and drop back down afterwards. Whether
the step actually happened is decided by which attempt covered more ground.
==================
*/
static void PM_StepSlideMove( qboolean gravity ) {
	vec3_t  start_o, start_v;
	vec3_t  down_o, down_v;
	trace_t trace;
	vec3_t  up, down;
	float   stepSize;

	VectorCopy( pm->ps->origin, start_o );
	VectorCopy( pm->ps->velocity, start_v );

	if ( PM_SlideMove( gravity ) == qfalse ) {
		return;   // nothing blocked us, we are done
	}

	VectorCopy( pm->ps->origin, down_o );
	VectorCopy( pm->ps->velocity, down_v );

	VectorCopy( start_o, up );
	up[2] += CF_STEP_HEIGHT;

	// test the player position if they were a stepheight higher
	pm->trace( &trace, start_o, pm->mins, pm->maxs, up, pm->ps->clientNum, pm->tracemask );
	if ( trace.allsolid ) {
		return;   // can't step up, there is a ceiling in the way
	}

	stepSize = trace.endpos[2] - start_o[2];

	// try slide move from this position
	VectorCopy( trace.endpos, pm->ps->origin );
	VectorCopy( start_v, pm->ps->velocity );

	PM_SlideMove( gravity );

	// push down the final amount
	VectorCopy( pm->ps->origin, down );
	down[2] -= stepSize;
	pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, down, pm->ps->clientNum, pm->tracemask );
	if ( !trace.allsolid ) {
		VectorCopy( trace.endpos, pm->ps->origin );
	}
	if ( trace.fraction < 1.0f ) {
		PM_ClipVelocity( pm->ps->velocity, trace.plane.normal, pm->ps->velocity, CF_OVERCLIP );
	}

	// if the stepped move did not get us further along the ground than the
	// plain one did, keep the plain one -- otherwise we "step" up walls
	if ( ( down_o[0] - start_o[0] ) * ( down_o[0] - start_o[0] ) +
	     ( down_o[1] - start_o[1] ) * ( down_o[1] - start_o[1] ) >
	     ( pm->ps->origin[0] - start_o[0] ) * ( pm->ps->origin[0] - start_o[0] ) +
	     ( pm->ps->origin[1] - start_o[1] ) * ( pm->ps->origin[1] - start_o[1] ) ) {
		VectorCopy( down_o, pm->ps->origin );
		VectorCopy( down_v, pm->ps->velocity );
	}
}

/*
===================
PM_WalkMove
===================
*/
static void PM_WalkMove( void ) {
	int    i;
	vec3_t wishvel;
	float  fmove, smove;
	vec3_t wishdir;
	float  wishspeed;
	float  scale;
	float  accelerate;
	float *vel;

	if ( PM_CheckJump() ) {
		PM_AirMove();
		return;
	}

	PM_Friction();

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;

	scale = PM_CmdScale( &pm->cmd );

	// project the view axes onto the ground plane, so that looking up or down
	// a slope does not change how fast you run along it
	pml.forward[2] = 0;
	pml.right[2] = 0;

	PM_ClipVelocity( pml.forward, pml.groundTrace.plane.normal, pml.forward, CF_OVERCLIP );
	PM_ClipVelocity( pml.right, pml.groundTrace.plane.normal, pml.right, CF_OVERCLIP );

	VectorNormalize( pml.forward );
	VectorNormalize( pml.right );

	for ( i = 0; i < 3; i++ ) {
		wishvel[i] = pml.forward[i] * fmove + pml.right[i] * smove;
	}

	VectorCopy( wishvel, wishdir );
	wishspeed = VectorNormalize( wishdir );
	wishspeed *= scale;

	wishspeed = PM_LimitWishSpeed( wishspeed );

	// slick surfaces get air acceleration, which is what makes ice feel like ice
	if ( pml.groundTrace.surfaceFlags & SURF_SLICK ) {
		accelerate = CF_AIR_ACCEL;
	} else {
		accelerate = CF_GROUND_ACCEL;
	}

	PM_Accelerate( wishdir, wishspeed, accelerate );

	vel = pm->ps->velocity;

	// on ice you keep falling; on ordinary ground the clip below flattens the
	// vertical component out anyway
	if ( pml.groundTrace.surfaceFlags & SURF_SLICK ) {
		vel[2] -= pm->ps->gravity * pml.frametime;
	}

	// keep velocity parallel to the ground so that walking down a slope does
	// not launch you off every lip
	PM_ClipVelocity( vel, pml.groundTrace.plane.normal, vel, CF_OVERCLIP );

	if ( !vel[0] && !vel[1] ) {
		return;
	}

	PM_StepSlideMove( qfalse );
}

/*
===================
PM_AirMove

Air control: steer toward where you are pointing, then clamp so that steering
cannot manufacture speed. See the CF_AIR_ACCEL comment in cf_shared.h -- the
clamp is the whole design decision.
===================
*/
static void PM_AirMove( void ) {
	int    i;
	vec3_t wishvel;
	float  fmove, smove;
	vec3_t wishdir;
	float  wishspeed;
	float  scale;
#if CF_AIR_CONTROL_CLAMP
	float  beforeSpeed, afterSpeed, cap;
	vec3_t horizontal;
#endif

	PM_Friction();

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;

	scale = PM_CmdScale( &pm->cmd );

	// project moves down to the horizontal plane
	pml.forward[2] = 0;
	pml.right[2] = 0;
	VectorNormalize( pml.forward );
	VectorNormalize( pml.right );

	for ( i = 0; i < 2; i++ ) {
		wishvel[i] = pml.forward[i] * fmove + pml.right[i] * smove;
	}
	wishvel[2] = 0;

	VectorCopy( wishvel, wishdir );
	wishspeed = VectorNormalize( wishdir );
	wishspeed *= scale;

	wishspeed = PM_LimitWishSpeed( wishspeed );

#if CF_AIR_CONTROL_CLAMP
	horizontal[0] = pm->ps->velocity[0];
	horizontal[1] = pm->ps->velocity[1];
	horizontal[2] = 0;
	beforeSpeed = VectorLength( horizontal );
#endif

	PM_Accelerate( wishdir, wishspeed, CF_AIR_ACCEL );

#if CF_AIR_CONTROL_CLAMP
	horizontal[0] = pm->ps->velocity[0];
	horizontal[1] = pm->ps->velocity[1];
	horizontal[2] = 0;
	afterSpeed = VectorLength( horizontal );

	// air control may redirect momentum and may bring you up to run speed, but
	// it may not add to speed you already had
	cap = beforeSpeed > pm->ps->speed ? beforeSpeed : pm->ps->speed;
	if ( afterSpeed > cap && afterSpeed > 0.0f ) {
		pm->ps->velocity[0] *= cap / afterSpeed;
		pm->ps->velocity[1] *= cap / afterSpeed;
	}
#endif

	// we may have a ground plane that is very steep, even though we are not
	// on the ground -- slide along it rather than into it
	if ( pml.groundPlane ) {
		PM_ClipVelocity( pm->ps->velocity, pml.groundTrace.plane.normal,
		                 pm->ps->velocity, CF_OVERCLIP );
	}

	PM_StepSlideMove( qtrue );
}

/*
===================
PM_DeadMove

Keep sliding to a stop, but no input.
===================
*/
static void PM_DeadMove( void ) {
	float forward;

	if ( !pml.walking ) {
		return;
	}

	forward = VectorLength( pm->ps->velocity );
	forward -= 20;
	if ( forward <= 0 ) {
		VectorClear( pm->ps->velocity );
	} else {
		VectorNormalize( pm->ps->velocity );
		VectorScale( pm->ps->velocity, forward, pm->ps->velocity );
	}
}

/*
===================
PM_NoclipMove
===================
*/
static void PM_NoclipMove( void ) {
	float  speed, drop, friction, control, newspeed;
	int    i;
	vec3_t wishvel;
	float  fmove, smove;
	vec3_t wishdir;
	float  wishspeed;
	float  scale;

	pm->ps->viewheight = CF_VIEWHEIGHT_STAND;

	// friction
	speed = VectorLength( pm->ps->velocity );
	if ( speed < 1 ) {
		VectorCopy( vec3_origin, pm->ps->velocity );
	} else {
		drop = 0;

		friction = CF_GROUND_FRICTION * 1.5f;   // extra friction, noclip is a debug tool
		control = speed < CF_STOP_SPEED ? CF_STOP_SPEED : speed;
		drop += control * friction * pml.frametime;

		newspeed = speed - drop;
		if ( newspeed < 0 ) {
			newspeed = 0;
		}
		newspeed /= speed;

		VectorScale( pm->ps->velocity, newspeed, pm->ps->velocity );
	}

	// accelerate
	scale = PM_CmdScale( &pm->cmd );

	fmove = pm->cmd.forwardmove;
	smove = pm->cmd.rightmove;

	for ( i = 0; i < 3; i++ ) {
		wishvel[i] = pml.forward[i] * fmove + pml.right[i] * smove;
	}
	wishvel[2] += pm->cmd.upmove;

	VectorCopy( wishvel, wishdir );
	wishspeed = VectorNormalize( wishdir );
	wishspeed *= scale;

	PM_Accelerate( wishdir, wishspeed, CF_GROUND_ACCEL );

	// move
	VectorMA( pm->ps->origin, pml.frametime, pm->ps->velocity, pm->ps->origin );
}

/*
=============
PM_CorrectAllSolid

We started the frame stuck inside something. Try nudging out along each axis
before giving up and declaring ourselves airborne.
=============
*/
static int PM_CorrectAllSolid( trace_t *trace ) {
	int     i, j, k;
	vec3_t  point;

	for ( i = -1; i <= 1; i++ ) {
		for ( j = -1; j <= 1; j++ ) {
			for ( k = -1; k <= 1; k++ ) {
				VectorCopy( pm->ps->origin, point );
				point[0] += (float)i;
				point[1] += (float)j;
				point[2] += (float)k;
				pm->trace( trace, point, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask );
				if ( !trace->allsolid ) {
					// found free space next to us -- redo the ground trace
					// from where we actually are
					point[0] = pm->ps->origin[0];
					point[1] = pm->ps->origin[1];
					point[2] = pm->ps->origin[2] - 0.25f;
					pm->trace( trace, pm->ps->origin, pm->mins, pm->maxs, point,
					           pm->ps->clientNum, pm->tracemask );
					pml.groundTrace = *trace;
					return qtrue;
				}
			}
		}
	}

	pm->ps->groundEntityNum = ENTITYNUM_NONE;
	pml.groundPlane = qfalse;
	pml.walking = qfalse;

	return qfalse;
}

/*
=============
PM_CrashLand

Called when we hit the ground with some downward speed.
=============
*/
static void PM_CrashLand( void ) {
	float delta;
	int   parm;

	delta = pml.impactSpeed;

	if ( pm->ps->pm_type == PM_DEAD ) {
		return;
	}

	/*
	The impact speed rides along as the event parameter, so the server can scale
	fall damage smoothly with how far you actually fell rather than in the same
	three steps the sound uses. eventParm is a byte on the wire, so it goes
	across in units of 8 -- about 8 units/sec of resolution, far finer than
	anything a player can feel.
	*/
	parm = (int)( delta / 8.0f );
	if ( parm > 255 ) {
		parm = 255;
	}

	if ( delta > 550 ) {
		PM_AddEventParm( EV_FALL_FAR, parm );
	} else if ( delta > 350 ) {
		PM_AddEventParm( EV_FALL_MEDIUM, parm );
	} else if ( delta > 180 ) {
		PM_AddEventParm( EV_FALL_SHORT, parm );
	}
}

/*
=============
PM_GroundTrace
=============
*/
static void PM_GroundTrace( void ) {
	vec3_t  point;
	trace_t trace;

	point[0] = pm->ps->origin[0];
	point[1] = pm->ps->origin[1];
	point[2] = pm->ps->origin[2] - 0.25f;

	pm->trace( &trace, pm->ps->origin, pm->mins, pm->maxs, point, pm->ps->clientNum, pm->tracemask );
	pml.groundTrace = trace;

	// do something corrective if the trace starts in a solid
	if ( trace.allsolid ) {
		if ( !PM_CorrectAllSolid( &trace ) ) {
			return;
		}
	}

	// if the trace didn't hit anything, we are in free fall
	if ( trace.fraction == 1.0f ) {
		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qfalse;
		pml.walking = qfalse;
		return;
	}

	// check if getting thrown off the ground
	if ( pm->ps->velocity[2] > 0 && DotProduct( pm->ps->velocity, trace.plane.normal ) > 10 ) {
		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qfalse;
		pml.walking = qfalse;
		return;
	}

	// slopes that are too steep will not be considered onground
	if ( trace.plane.normal[2] < CF_MIN_WALK_NORMAL ) {
		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		pml.groundPlane = qtrue;
		pml.walking = qfalse;
		return;
	}

	pml.groundPlane = qtrue;
	pml.walking = qtrue;

	// hitting solid ground will end a waterjump / count as a landing
	if ( pm->ps->groundEntityNum == ENTITYNUM_NONE ) {
		PM_CrashLand();
	}

	pm->ps->groundEntityNum = trace.entityNum;

	PM_AddTouchEnt( trace.entityNum );
}

/*
==============
PM_CheckDuck
==============
*/
static void PM_CheckDuck( void ) {
	trace_t trace;
	vec3_t  standMins, standMaxs;

	if ( pm->cmd.upmove < 0 ) {
		pm->ps->pm_flags |= PMF_DUCKED;
	} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
		// try to stand up -- only allowed when there is room above
		standMins[0] = CF_MINS_X;
		standMins[1] = CF_MINS_Y;
		standMins[2] = CF_MINS_Z;
		standMaxs[0] = CF_MAXS_X;
		standMaxs[1] = CF_MAXS_Y;
		standMaxs[2] = CF_MAXS_Z;

		pm->trace( &trace, pm->ps->origin, standMins, standMaxs, pm->ps->origin,
		           pm->ps->clientNum, pm->tracemask );
		if ( !trace.allsolid ) {
			pm->ps->pm_flags &= ~PMF_DUCKED;
		}
	}

	CF_PlayerBounds( pm->ps, pm->mins, pm->maxs );

	if ( pm->ps->pm_type == PM_DEAD ) {
		pm->ps->viewheight = CF_VIEWHEIGHT_DEAD;
	} else if ( pm->ps->pm_flags & PMF_DUCKED ) {
		pm->ps->viewheight = CF_VIEWHEIGHT_CROUCH;
	} else {
		pm->ps->viewheight = CF_VIEWHEIGHT_STAND;
	}
}

/*
================
PM_SetMovementDir

Which of the eight compass directions the player is moving relative to where
they are looking. Used to pick strafe animations; harmless to compute now.
================
*/
static void PM_SetMovementDir( void ) {
	if ( pm->cmd.forwardmove || pm->cmd.rightmove ) {
		if ( pm->cmd.rightmove == 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 0;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 1;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove == 0 ) {
			pm->ps->movementDir = 2;
		} else if ( pm->cmd.rightmove < 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 3;
		} else if ( pm->cmd.rightmove == 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 4;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove < 0 ) {
			pm->ps->movementDir = 5;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove == 0 ) {
			pm->ps->movementDir = 6;
		} else if ( pm->cmd.rightmove > 0 && pm->cmd.forwardmove > 0 ) {
			pm->ps->movementDir = 7;
		}
	}
}

/*
================
CF_UpdateViewAngles

The client sends absolute view angles; the server adds delta_angles, which is
how spawns and teleporters get to reorient you without the client fighting it.
Pitch is clamped so you cannot look past straight up or down.
================
*/
void CF_UpdateViewAngles( playerState_t *ps, const usercmd_t *cmd ) {
	short temp;
	int   i;

	if ( ps->pm_type == PM_INTERMISSION || ps->pm_type == PM_FREEZE ) {
		return;   // no view changes at all
	}

	for ( i = 0; i < 3; i++ ) {
		temp = cmd->angles[i] + ps->delta_angles[i];
		if ( i == PITCH ) {
			if ( temp > 16000 ) {
				ps->delta_angles[i] = 16000 - cmd->angles[i];
				temp = 16000;
			} else if ( temp < -16000 ) {
				ps->delta_angles[i] = -16000 - cmd->angles[i];
				temp = -16000;
			}
		}
		ps->viewangles[i] = SHORT2ANGLE( temp );
	}
}

/*
==============================================================================

WEAPONS

All of this runs inside pmove, which means it runs on the client (predicting)
and on the server (deciding) from the same code and the same inputs. That is
what makes pulling the trigger feel instant rather than feeling like a request:
the shot, the recoil and the empty click all happen on the frame the button
went down, and the server independently arrives at the same answer.

Nothing here decides what a bullet HIT. Pmove says a shot happened; the server
traces it in g_weapon.c. A client that lies about firing gets nothing, because
its own trace is never consulted.

Everything is integer arithmetic in fixed units -- hundredths of a degree,
milliseconds -- because the two sides have to agree exactly and floats drift.

==============================================================================
*/

/*
================
PM_RecoilShort

The delta_angles offset, in engine angle units, for a given amount of
accumulated muzzle rise.

Recoil is applied as the DIFFERENCE between two of these rather than as a
per-shot increment, so the rounding cannot accumulate. When the rise returns to
zero, the total applied is exactly zero and the view is back where the player
left it -- not a hundredth of a degree low after thirty shots.
================
*/
static int PM_RecoilShort( int hundredthsOfADegree ) {
	return ( hundredthsOfADegree * 65536 ) / 36000;
}

static void PM_SetRecoil( int newRecoil ) {
	int before = PM_RecoilShort( pm->ps->stats[STAT_RECOIL] );
	int after  = PM_RecoilShort( newRecoil );

	// Negative pitch is up.
	pm->ps->delta_angles[PITCH] -= ( after - before );
	pm->ps->stats[STAT_RECOIL] = newRecoil;
}

/*
================
PM_ShotJitter

A deterministic pseudo-random deflection in [-range, range].

Seeded from the command time, so the client and the server compute the same
number for the same shot. A real random() here would put the two sides on
different answers and show up as the view snapping when a prediction error was
corrected.
================
*/
static int PM_ShotJitter( int seed, int range ) {
	unsigned int s = (unsigned int)seed;

	if ( range <= 0 ) {
		return 0;
	}
	s = s * 1103515245u + 12345u;
	s = ( s >> 16 ) & 0x7fffu;
	return (int)( s % (unsigned int)( range * 2 + 1 ) ) - range;
}

static void PM_RecoverRecoil( void ) {
	const cf_weaponInfo_t *w = CF_Weapon( pm->ps->weapon );
	int recovered;

	if ( pm->ps->stats[STAT_RECOIL] <= 0 ) {
		pm->ps->stats[STAT_RECOIL] = 0;
		return;
	}

	recovered = ( w->recoilRecover * pml.msec ) / 1000;
	if ( recovered < 1 ) {
		// Below a whole unit per frame the rise would never come down at high
		// frame rates. Move at least one unit whenever there is any left.
		recovered = 1;
	}
	if ( recovered > pm->ps->stats[STAT_RECOIL] ) {
		recovered = pm->ps->stats[STAT_RECOIL];
	}

	PM_SetRecoil( pm->ps->stats[STAT_RECOIL] - recovered );
}

/*
================
PM_StartReload / PM_FinishReload

A reload has two cases and they are meant to feel different.

TACTICAL, with rounds still in the gun: the slide never locked back, so the
chambered round is still there. Drop the magazine, seat a fresh one, and the
gun ends up holding a full magazine PLUS that chambered round.

EMPTY, from slide-lock: the same magazine change, plus releasing the slide to
chamber a round. That is the extra half second, and the gun ends up holding
exactly a magazine and no more.

Being caught doing the slow one is a real cost that real shooters plan around,
and reproducing it is most of what makes reloading interesting rather than a
pause.

Reserve is a pool of loose rounds, so a partial magazine's contents are kept
rather than thrown away with the magazine. That is the forgiving convention and
it is a deliberate choice -- the harsher one, where dropping a half-full mag
loses those rounds, is a single change here if it is ever wanted.
================
*/
static qboolean PM_StartReload( void ) {
	const cf_weaponInfo_t *w = CF_Weapon( pm->ps->weapon );
	int inGun    = pm->ps->ammo[pm->ps->weapon];
	int capacity = w->magazine + ( w->chambersRound ? 1 : 0 );

	if ( pm->ps->stats[STAT_RESERVE] <= 0 || inGun >= capacity ) {
		return qfalse;
	}

	if ( inGun <= 0 ) {
		pm->ps->weaponTime = w->reloadEmpty;
		PM_AddEventParm( EV_RELOAD, 1 );   // slide was locked back
	} else {
		pm->ps->weaponTime = w->reloadTactical;
		PM_AddEventParm( EV_RELOAD, 0 );
	}
	pm->ps->weaponstate = WEAPON_RELOADING;
	return qtrue;
}

static void PM_FinishReload( void ) {
	const cf_weaponInfo_t *w = CF_Weapon( pm->ps->weapon );
	int inGun = pm->ps->ammo[pm->ps->weapon];
	int want;
	int take;

	if ( inGun > 0 && w->chambersRound ) {
		want = w->magazine + 1;
	} else {
		want = w->magazine;
	}

	take = want - inGun;
	if ( take > pm->ps->stats[STAT_RESERVE] ) {
		take = pm->ps->stats[STAT_RESERVE];
	}
	if ( take < 0 ) {
		take = 0;
	}

	pm->ps->ammo[pm->ps->weapon] += take;
	pm->ps->stats[STAT_RESERVE]  -= take;
	PM_AddEvent( EV_RELOAD_DONE );
}

/*
================
PM_Weapon
================
*/
static void PM_Weapon( void ) {
	const cf_weaponInfo_t *w;

	if ( pm->ps->weapon == WP_NONE ) {
		return;
	}

	// Being dead is not a state you shoot from, but the rise still settles --
	// otherwise it would be waiting, banked, on the next spawn.
	if ( pm->ps->pm_type != PM_NORMAL ) {
		PM_RecoverRecoil();
		return;
	}

	w = CF_Weapon( pm->ps->weapon );

	PM_RecoverRecoil();

	if ( pm->ps->weaponTime > 0 ) {
		pm->ps->weaponTime -= pml.msec;
		if ( pm->ps->weaponTime <= 0 ) {
			pm->ps->weaponTime = 0;
			if ( pm->ps->weaponstate == WEAPON_RELOADING ) {
				PM_FinishReload();
			}
			pm->ps->weaponstate = WEAPON_READY;
		}
	}

	// Trigger and reload key reset. Releasing is what re-arms them; see
	// PMF_ATTACK_HELD for why a semi-automatic depends on it.
	if ( !( pm->cmd.buttons & BUTTON_ATTACK ) ) {
		pm->ps->pm_flags &= ~PMF_ATTACK_HELD;
	}
	if ( !( pm->cmd.buttons & BUTTON_USE_HOLDABLE ) ) {
		pm->ps->pm_flags &= ~PMF_RELOAD_HELD;
	}

	if ( pm->ps->weaponTime > 0 ) {
		return;   // mid-cycle or mid-reload
	}

	if ( ( pm->cmd.buttons & BUTTON_USE_HOLDABLE )
	     && !( pm->ps->pm_flags & PMF_RELOAD_HELD ) ) {
		pm->ps->pm_flags |= PMF_RELOAD_HELD;
		if ( PM_StartReload() ) {
			return;
		}
	}

	if ( !( pm->cmd.buttons & BUTTON_ATTACK ) ) {
		pm->ps->weaponstate = WEAPON_READY;
		return;
	}

	if ( w->fireMode == FIRE_SEMI && ( pm->ps->pm_flags & PMF_ATTACK_HELD ) ) {
		return;   // the trigger has not reset
	}
	pm->ps->pm_flags |= PMF_ATTACK_HELD;

	if ( pm->ps->ammo[pm->ps->weapon] <= 0 ) {
		// The click of an empty gun. Deliberately audible: on a pistol whose
		// slide has locked back it is the clearest "you are out" there is.
		PM_AddEvent( EV_DRY_FIRE );
		pm->ps->weaponTime = 250;
		return;
	}

	pm->ps->ammo[pm->ps->weapon]--;
	PM_AddEvent( EV_FIRE );
	pm->ps->weaponstate = WEAPON_FIRING;
	pm->ps->weaponTime = w->fireInterval;

	{
		int rise = pm->ps->stats[STAT_RECOIL] + w->recoilPitch;

		if ( rise > w->recoilMax ) {
			rise = w->recoilMax;
		}
		PM_SetRecoil( rise );

		// Sideways deflection is applied straight to the view and never
		// recovered, because a pistol has no memorised spray pattern to learn
		// -- it wanders, and that is the shooter's problem to hold.
		pm->ps->delta_angles[YAW] +=
			PM_RecoilShort( PM_ShotJitter( pm->cmd.serverTime, w->recoilYaw ) );
	}
}

/*
================
PmoveSingle
================
*/
static void PmoveSingle( pmove_t *pmove ) {
	pm = pmove;

	pm->numtouch = 0;

	// BUTTON_ANY is set by the engine for any keypress; clear it so that a
	// respawn-on-any-key does not fire off a stale button
	if ( pm->ps->pm_type >= PM_DEAD ) {
		pm->cmd.buttons &= ~BUTTON_ANY;
		pm->cmd.forwardmove = 0;
		pm->cmd.rightmove = 0;
		pm->cmd.upmove = 0;
	}

	// clear the results
	memset( &pml, 0, sizeof( pml ) );

	pml.msec = pmove->cmd.serverTime - pm->ps->commandTime;
	if ( pml.msec < 1 ) {
		pml.msec = 1;
	} else if ( pml.msec > 200 ) {
		pml.msec = 200;
	}
	pm->ps->commandTime = pmove->cmd.serverTime;

	VectorCopy( pm->ps->origin, pml.previous_origin );
	VectorCopy( pm->ps->velocity, pml.previous_velocity );

	pml.frametime = pml.msec * 0.001f;

	CF_UpdateViewAngles( pm->ps, &pm->cmd );

	AngleVectors( pm->ps->viewangles, pml.forward, pml.right, pml.up );

	if ( pm->cmd.upmove < 10 ) {
		pm->ps->pm_flags &= ~PMF_JUMP_HELD;
	}

	if ( pm->ps->pm_type == PM_NOCLIP ) {
		PM_NoclipMove();
		pm->ps->groundEntityNum = ENTITYNUM_NONE;
		return;
	}

	if ( pm->ps->pm_type == PM_FREEZE || pm->ps->pm_type == PM_INTERMISSION ) {
		return;   // no movement at all
	}

	// set mins, maxs and viewheight
	PM_CheckDuck();

	// set groundentity
	PM_GroundTrace();

	if ( pm->ps->pm_type == PM_DEAD ) {
		PM_DeadMove();
	}

	if ( pml.walking ) {
		PM_WalkMove();
	} else {
		PM_AirMove();
	}

	// re-check the ground, so that the state we report matches where we
	// actually ended up this frame
	PM_GroundTrace();

	PM_SetMovementDir();

	// After the move, so that "was I moving when I fired" is answered by where
	// this frame actually ended up rather than where the last one did.
	PM_Weapon();

	// snap velocity to integers, so the client's prediction and the server's
	// authoritative move cannot drift apart through float rounding
	SnapVector( pm->ps->velocity );
}

/*
================
CF_Pmove

Splits a long command into steps small enough that the physics stay stable, and
runs each one.
================
*/
void CF_Pmove( pmove_t *pmove ) {
	int finalTime;

	finalTime = pmove->cmd.serverTime;

	if ( finalTime < pmove->ps->commandTime ) {
		return;   // should not happen
	}

	if ( finalTime > pmove->ps->commandTime + 1000 ) {
		pmove->ps->commandTime = finalTime - 1000;
	}

	pmove->ps->pmove_framecount = ( pmove->ps->pmove_framecount + 1 ) & ( ( 1 << PS_PMOVEFRAMECOUNTBITS ) - 1 );

	// chop the move up into steps of at most 66ms
	while ( pmove->ps->commandTime != finalTime ) {
		int msec;

		msec = finalTime - pmove->ps->commandTime;
		if ( msec > 66 ) {
			msec = 66;
		}

		pmove->cmd.serverTime = pmove->ps->commandTime + msec;
		PmoveSingle( pmove );
	}
}
