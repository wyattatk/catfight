/*
===========================================================================
catfight -- building the view and drawing a frame.
===========================================================================
*/

#include "cg_local.h"

/*
=================
CG_CalcVrect

catfight always renders the full window. Quake 3 shrank the 3D view for its
"screen size" option, which existed because software rendering was slow; there
is no reason to carry that forward.
=================
*/
static void CG_CalcVrect( void ) {
	cg.refdef.x = 0;
	cg.refdef.y = 0;
	cg.refdef.width = cgs.glconfig.vidWidth;
	cg.refdef.height = cgs.glconfig.vidHeight;
}

/*
====================
CG_CalcFov

cg_fov is the horizontal field of view at 4:3. The vertical is derived from the
actual window aspect, so a wider window shows more at the sides rather than
squashing what is already there.
====================
*/
static void CG_CalcFov( void ) {
	float x;
	float fov_x, fov_y;
	float aspect;

	fov_x = cg_fov.value;
	if ( fov_x < 1 ) {
		fov_x = 1;
	} else if ( fov_x > 160 ) {
		fov_x = 160;
	}

	// widen the horizontal fov on wider-than-4:3 windows, keeping the vertical
	// fov fixed, so that widescreen shows more instead of less
	aspect = (float)cg.refdef.width / (float)cg.refdef.height;

	x = ( 640.0f / 480.0f ) / tan( fov_x / 360 * M_PI );
	fov_y = atan2( 480.0f / 640.0f * aspect, x );
	fov_y = fov_y * 360 / M_PI;

	x = cg.refdef.height / tan( fov_y / 360 * M_PI );
	fov_x = atan2( cg.refdef.width, x );
	fov_x = fov_x * 360 / M_PI;

	cg.refdef.fov_x = fov_x;
	cg.refdef.fov_y = fov_y;
	cg.fov = fov_x;
}

/*
===============
CG_OffsetThirdPersonView
===============
*/
static void CG_OffsetThirdPersonView( void ) {
	vec3_t  forward, right, up;
	vec3_t  view;
	trace_t trace;
	vec3_t  mins = { -4, -4, -4 };
	vec3_t  maxs = { 4, 4, 4 };

	AngleVectors( cg.refdefViewAngles, forward, right, up );

	VectorMA( cg.refdef.vieworg, -cg_thirdPersonRange.value, forward, view );
	view[2] += 16;

	// don't let the camera end up inside a wall
	CG_Trace( &trace, cg.refdef.vieworg, mins, maxs, view, cg.predictedPlayerState.clientNum, MASK_SOLID );
	if ( trace.fraction != 1.0f ) {
		VectorCopy( trace.endpos, view );
	}

	VectorCopy( view, cg.refdef.vieworg );
}

/*
===============
CG_DeathCamera

For the moment after we die (CF_DEATH_BEAT_MS, the time the server holds us
dead), the camera comes out of our head: back and up behind where we fell,
looking down at our own body going over, then swinging round to face whoever
killed us so the last thing seen is them.

Starts from the way we were facing, so the cut out of first person is a pull
back rather than a jump. Traced from the body outward and stopped short of any
wall, as the third-person view is, or a death in a corner would put the camera
inside it.

Returns qfalse when there is no death to look at, and the ordinary view is
used.
===============
*/
#define CF_DEATHCAM_DIST     150.0f
// Shallow on purpose: steeper looks down at the body nicely and puts a killer
// standing across the room up at the top of the frame, under the HUD text.
#define CF_DEATHCAM_PITCH     12.0f
#define CF_DEATHCAM_TURN_AT  0.45f    // seconds before the turn toward the killer starts
#define CF_DEATHCAM_TURN     0.9f     // and how long it takes

static qboolean CG_DeathCamera( const playerState_t *ps ) {
	vec3_t  target, angles, forward, cam, mins = { -4, -4, -4 }, maxs = { 4, 4, 4 };
	float   t, s, yaw;
	trace_t trace;

	if ( ps->pm_type != PM_DEAD ) {
		cg.deathTime = 0;
		return qfalse;
	}
	if ( !cg.deathTime ) {
		return qfalse;   // dead, but we never saw it happen (joined dead)
	}

	t = (float)( cg.time - cg.deathTime ) * 0.001f;

	// Low on the body: it is lying down for most of this.
	VectorCopy( ps->origin, target );
	target[2] += CF_MINS_Z + 16;

	yaw = ps->viewangles[YAW];
	/*
	Turned toward the killer only when they are actually in our snapshot. A
	killer we are not being sent -- out of sight, or a spectator by now -- has
	a stale position, and swinging the camera to face a place nobody is would
	be worse than holding the view we died with.
	*/
	if ( cg.killer >= 0 && cg.killer < MAX_CLIENTS && cg.killer != ps->clientNum
	     && cg_entities[cg.killer].currentValid ) {
		vec3_t toKiller, killerAngles;
		float  killerYaw;

		VectorSubtract( cg_entities[cg.killer].lerpOrigin, target, toKiller );
		vectoangles( toKiller, killerAngles );
		killerYaw = killerAngles[YAW];

		s = ( t - CF_DEATHCAM_TURN_AT ) / CF_DEATHCAM_TURN;
		s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
		s = s * s * ( 3.0f - 2.0f * s );
		yaw += AngleSubtract( killerYaw, yaw ) * s;
	}

	VectorSet( angles, CF_DEATHCAM_PITCH, yaw, 0 );
	AngleVectors( angles, forward, NULL, NULL );

	// Pull out over the first half second rather than jumping there.
	s = t / 0.5f;
	s = s > 1.0f ? 1.0f : s;
	s = s * ( 2.0f - s );
	VectorMA( target, -CF_DEATHCAM_DIST * s, forward, cam );

	CG_Trace( &trace, target, mins, maxs, cam, ps->clientNum, MASK_SOLID );
	VectorCopy( trace.endpos, cg.refdef.vieworg );
	VectorCopy( angles, cg.refdefViewAngles );
	return qtrue;
}

/*
===============
CG_CalcViewValues
===============
*/
static void CG_CalcViewValues( void ) {
	playerState_t *ps;

	memset( &cg.refdef, 0, sizeof( cg.refdef ) );

	CG_CalcVrect();

	ps = &cg.predictedPlayerState;

	cg.deathCam = CG_DeathCamera( ps );
	if ( !cg.deathCam ) {
		VectorCopy( ps->origin, cg.refdef.vieworg );
		VectorCopy( ps->viewangles, cg.refdefViewAngles );

		cg.refdef.vieworg[2] += ps->viewheight;
	}

	AnglesToAxis( cg.refdefViewAngles, cg.refdef.viewaxis );

	if ( cg_thirdPerson.integer && !cg.deathCam ) {
		CG_OffsetThirdPersonView();
	}

	CG_CalcFov();
}

/*
=================
CG_DrawActiveFrame

The one function the engine calls to produce a frame.
=================
*/
void CG_DrawActiveFrame( int serverTime, stereoFrame_t stereoView, qboolean demoPlayback ) {
	(void)stereoView;

	cg.time = serverTime;
	cg.demoPlayback = demoPlayback;

	CG_UpdateCvars();

	CG_ProcessSnapshots();

	// no snapshot yet: we are still connecting or loading
	if ( !cg.snap || ( cg.snap->snapFlags & SNAPFLAG_NOT_ACTIVE ) ) {
		CG_DrawInformation();
		return;
	}

	cg.frametime = cg.time - cg.oldTime;
	if ( cg.frametime < 0 ) {
		cg.frametime = 0;
	}
	cg.oldTime = cg.time;

	CG_PredictPlayerState();

	/*
	Our own events, immediately after prediction and before anything reads the
	state -- this is the frame they happened on.

	The first frame is recorded and NOT fired. oldPlayerState is zeroed until
	then, so every slot in the ring would read as new and a fresh spawn would
	open with a burst of whatever the previous life did.
	*/
	if ( cg.validOPS ) {
		CG_CheckPlayerstateEvents( &cg.predictedPlayerState, &cg.oldPlayerState );
	} else {
		cg.validOPS = qtrue;
	}
	cg.oldPlayerState = cg.predictedPlayerState;

	// after prediction, because the result it records is read off the player
	// state, and before the draw, because it owns what the draw shows
	CG_UpdatePostgame();

	// after prediction for the same reason: she is told the situation the
	// player is actually in this frame, not the one before it
	CG_Voice_Frame();

	trap_R_ClearScene();
	trap_S_ClearLoopingSounds( qfalse );

	CG_CalcViewValues();

	CG_AddPacketEntities();

	// The death camera is looking at our own body, which the server never sends.
	if ( cg.deathCam ) {
		CG_AddOwnBody();
	}

	// Last of the scene. Our own muzzle flash is drawn from in here, at the
	// view model's muzzle; a dynamic light added anywhere in a scene lights
	// all of it, so where in the frame it is added does not matter.
	CG_AddViewWeapon();

	cg.refdef.time = cg.time;
	memcpy( cg.refdef.areamask, cg.snap->areamask, sizeof( cg.refdef.areamask ) );

	trap_R_RenderScene( &cg.refdef );

	// the listener follows the eye
	trap_S_Respatialize( cg.snap->ps.clientNum, cg.refdef.vieworg, cg.refdef.viewaxis, 0 );

	CG_Draw2D();
}
