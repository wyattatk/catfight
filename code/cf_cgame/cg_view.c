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
CG_CalcViewValues
===============
*/
static void CG_CalcViewValues( void ) {
	playerState_t *ps;

	memset( &cg.refdef, 0, sizeof( cg.refdef ) );

	CG_CalcVrect();

	ps = &cg.predictedPlayerState;

	VectorCopy( ps->origin, cg.refdef.vieworg );
	VectorCopy( ps->viewangles, cg.refdefViewAngles );

	cg.refdef.vieworg[2] += ps->viewheight;

	AnglesToAxis( cg.refdefViewAngles, cg.refdef.viewaxis );

	if ( cg_thirdPerson.integer ) {
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

	// after prediction, because the result it records is read off the player
	// state, and before the draw, because it owns what the draw shows
	CG_UpdatePostgame();

	trap_R_ClearScene();
	trap_S_ClearLoopingSounds( qfalse );

	CG_CalcViewValues();

	CG_AddPacketEntities();

	cg.refdef.time = cg.time;
	memcpy( cg.refdef.areamask, cg.snap->areamask, sizeof( cg.refdef.areamask ) );

	trap_R_RenderScene( &cg.refdef );

	// the listener follows the eye
	trap_S_Respatialize( cg.snap->ps.clientNum, cg.refdef.vieworg, cg.refdef.viewaxis, 0 );

	CG_Draw2D();
}
