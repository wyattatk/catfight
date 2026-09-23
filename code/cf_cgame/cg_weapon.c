/*
===========================================================================
catfight -- the weapon in your hands.

THIS FILE DRAWS NOTHING THAT MATTERS AND EVERYTHING THAT IS FELT.

Not one number here reaches the server, changes a trace, or moves a bullet. The
gun is drawn where it looks right, and where it looks right is not where it is:
a real pistol held at eye level would cover a third of the screen and sit
between you and everything you are trying to shoot. Every shooter lies about
this, and the lie is the feature.

So what this file is actually tuning is the sense that you are HOLDING
something -- weight, lag, a kick when it goes off, a dip when you reload. The
crosshair is where the bullet goes; the gun is how it feels to send one.

WHAT IT IS DRAWING RIGHT NOW is a box pistol from mapping/gen_gun_iqm.go, which
is the right size and the right shape and nothing else. That is deliberate and
it is enough: position, scale and motion all tune against a silhouette, and none
of that work is lost when a real model arrives at the same path.

IT NOW PLAYS ONE ANIMATION: the slide, on the `fire` clip the model carries.
Everything else here is still procedural -- bob, sway, recoil, the reload dip
-- and moves the whole gun as one rigid object. The reload dip in particular
is still standing in for an animation that does not exist yet, and says so
where it is written.

A model with no animation in it is unaffected: frame and oldframe both land on
0 and the renderer draws the bind pose, which is what the generated
placeholder has always shown.
===========================================================================
*/

#include "cg_local.h"

/*
MODEL SCALE.

THE HONEST VERSION OF THIS NUMBER: the world has no consistent physical scale to
convert against. A catfight player is 56 units tall and 32 wide with their eye
at 26 -- proportions inherited from Quake 3, where they were chosen to make
movement feel right and never to measure a person. Anchoring on eye height gives
about 16 units per metre; anchoring on shoulder width gives about 70. They
cannot both be true, so there is no conversion to derive and anything claiming
otherwise is arithmetic dressed up as authority.

What there IS is a number that makes the gun look the right size on screen,
found by looking at it. That is what cg_gunScale is, and it is a cvar because
the person tuning it should not have to rebuild.

MODELS ARE AUTHORED IN METRES, so cg_gunScale is applied directly and the name
means exactly what it says: world units per metre.

It used to say that and not do it. The convention was millimetres, and a
CF_MM_TO_UNITS macro divided by 1000 here to make up the difference -- so the
cvar was documented in one unit and applied in another, and the only way to
know was to read this file. Changed 2026-09-22 with the first real model, which
came in metres because metres is what Blender does by default and what every
other engine and interchange format uses. A convention that fights the tool's
default gets violated by every artist forever; this one had to be argued for in
#art-spec with a warning attached, which was the tell.

The arithmetic is unchanged -- 204mm at 38/1000 and 0.204m at 38 are the same
number -- so the default did not move.
*/

/*
Bob. Both are the offset at a full run, in units, before the model scale.

Deliberately small. Weapon bob is the effect most often overdone and the one
players turn off first when it is, and the reason is that it competes with aim:
anything large enough to notice while shooting is large enough to annoy. It
should be visible when you are running and invisible when you stop.
*/
#define CF_GUN_BOB_UP    0.60f
#define CF_GUN_BOB_SIDE  0.45f

// How fast the bob amplitude follows the player's speed, and the dip follows a
// reload. A time constant in milliseconds: roughly how long to cover most of
// the distance to the target value.
#define CF_GUN_FOLLOW_MS 120.0f

/*
Sway -- the gun trailing the view when you turn.

CF_GUN_SWAY_MAX caps it in degrees, because without a cap a fast flick sends the
gun off the side of the screen and back, which reads as a bug rather than as
weight. CF_GUN_SWAY_MS is how long it takes to catch up.
*/
#define CF_GUN_SWAY_MAX  6.0f
#define CF_GUN_SWAY_MS   90.0f

// How far the sway angle also shifts the gun sideways and vertically, in units
// per degree. Rotation alone reads as the gun pivoting on the muzzle; a little
// translation with it reads as the whole arm moving.
#define CF_GUN_SWAY_SHIFT 0.18f

/*
Recoil, as a multiple of the accumulated rise the player state already carries.

STAT_RECOIL is the real thing -- predicted in pmove, identical on the server, and
already applied to the view angles, so the gun rises with the camera for free.
What these add is the gun moving RELATIVE to the view, which is the part that
reads as the weapon recoiling rather than the player looking up.
*/
#define CF_GUN_RECOIL_PITCH 0.55f
#define CF_GUN_RECOIL_BACK  0.09f

// How far the gun drops out of frame, in units, while being raised.
#define CF_GUN_RAISE_DROP  4.5f

/*
THE RELOAD PRESENT: how the gun is turned to show the magazine well while the
magazine clip plays.

At rest the bottom of the grip is below the edge of the screen, so a magazine
leaving it would never be seen -- the first try at this dropped the gun
instead, and the magazine left a gun whose grip was already out of shot. So it
comes UP a little, tips its nose up (which swings the base of the grip forward
and up into view), rolls in and slides toward the middle, and the magazine is
seen going out and coming back. Units and degrees, tuned by looking.
*/
#define CF_GUN_RELOAD_LIFT   1.5f
#define CF_GUN_RELOAD_IN     2.5f
#define CF_GUN_RELOAD_PITCH 22.0f
#define CF_GUN_RELOAD_ROLL  28.0f

/*
===============
CG_ExpFollow

One-pole smoothing toward a target, framerate independent.

Written out rather than inlined three times because the naive version --
value += (target - value) * 0.1f -- is framerate DEPENDENT, so the gun settles
at a different speed at 60fps and at 250fps. That is the kind of thing that gets
tuned to feel right on the machine it was written on and feels wrong everywhere
else, and a shooter is exactly where somebody will notice.
===============
*/
static float CG_ExpFollow( float value, float target, float tauMs ) {
	float lambda;

	if ( tauMs <= 0.0f || cg.frametime <= 0 ) {
		return target;
	}

	lambda = 1.0f - exp( -(float)cg.frametime / tauMs );
	return value + ( target - value ) * lambda;
}

/*
===============
CG_WeaponSway

Accumulate how far the view has turned since last frame and let the gun fall
that far behind, then decay it back to zero so it catches up.

The sign is the whole trick: the sway accumulates the NEGATIVE of the view
delta, so that viewangles + sway is where the view was pointing a moment ago.
Adding the delta instead gives a gun that leads the turn, which looks like it is
being thrown rather than carried.
===============
*/
static void CG_WeaponSway( vec3_t out ) {
	int i;

	if ( !cg.gunValid ) {
		VectorCopy( cg.refdefViewAngles, cg.gunOldAngles );
		cg.gunValid = qtrue;
	}

	for ( i = 0; i < 3; i++ ) {
		float delta = AngleSubtract( cg.refdefViewAngles[i], cg.gunOldAngles[i] );

		cg.gunSway[i] -= delta;

		if ( cg.gunSway[i] > CF_GUN_SWAY_MAX ) {
			cg.gunSway[i] = CF_GUN_SWAY_MAX;
		} else if ( cg.gunSway[i] < -CF_GUN_SWAY_MAX ) {
			cg.gunSway[i] = -CF_GUN_SWAY_MAX;
		}

		cg.gunSway[i] = CG_ExpFollow( cg.gunSway[i], 0.0f, CF_GUN_SWAY_MS );
	}

	VectorCopy( cg.refdefViewAngles, cg.gunOldAngles );
	VectorCopy( cg.gunSway, out );
}

/*
===============
CG_WeaponBob

Phase from the player state, amplitude from the live speed, and they come from
different places on purpose.

ps->bobCycle is replicated and predicted, so the phase is the same number the
server has and the same one that fires footsteps -- the gun dips on the step you
hear rather than near it.

But PM_Footsteps RETURNS EARLY when ducked, walking or airborne, so the cycle
simply stops advancing in all three. Driving amplitude off the cycle too would
freeze the gun mid-swing at whatever phase it happened to be holding, which
looks like the game hung. Taking amplitude from the current horizontal speed
instead means the bob fades out smoothly whenever the phase stalls, and the
frozen phase is invisible because it is being multiplied by nothing.
===============
*/
static void CG_WeaponBob( const playerState_t *ps, float *upOut, float *rightOut ) {
	float phase;
	float speed;
	float target;

	speed = sqrt( ps->velocity[0] * ps->velocity[0] + ps->velocity[1] * ps->velocity[1] );

	target = speed / CF_RUN_SPEED;
	if ( target > 1.0f ) {
		target = 1.0f;
	}
	if ( ps->groundEntityNum == ENTITYNUM_NONE ) {
		target = 0.0f;       // nothing to push off: in the air the gun is still
	}

	cg.gunBob = CG_ExpFollow( cg.gunBob, target, CF_GUN_FOLLOW_MS );

	// bobCycle wraps at 256 over a full two-step cycle
	phase = (float)ps->bobCycle / 256.0f * 2.0f * M_PI;

	// Side to side once per cycle, down once per STEP -- fabs halves the period,
	// which is what puts a dip under each foot rather than under every other one.
	*rightOut = sin( phase ) * cg.gunBob * CF_GUN_BOB_SIDE;
	*upOut = -fabs( sin( phase ) ) * cg.gunBob * CF_GUN_BOB_UP;
}

/*
===============
CG_WeaponReloadFrame

Pose a weapon model's magazine for a reload `progress` of the way through,
0..1; anything outside that leaves the frames alone. Shared by your own gun and
the one in everybody else's hands, so both play the one clip on the one
schedule.

It takes over from the slide for as long as it runs. The two clips key every
bone, so a frame of one cannot be blended with a frame of the other -- and
during a reload the slide has nothing to do anyway.
===============
*/
void CG_WeaponReloadFrame( float progress, refEntity_t *gun ) {
	float t;
	int   frame;

	if ( progress < 0.0f || progress >= 1.0f ) {
		return;
	}
	t = progress * (float)( CF_MAG_COUNT - 1 );
	frame = (int)t;
	if ( frame >= CF_MAG_COUNT - 1 ) {
		frame = CF_MAG_COUNT - 2;
	}
	gun->oldframe = CF_MAG_FIRST + frame;
	gun->frame = CF_MAG_FIRST + frame + 1;
	gun->backlerp = 1.0f - ( t - (float)frame );
}

/*
===============
CG_AddViewWeapon

Build the first-person weapon and add it to the scene.
===============
*/
void CG_AddViewWeapon( void ) {
	const playerState_t   *ps;
	const cf_weaponInfo_t *w;
	refEntity_t            gun;
	vec3_t                 angles;
	vec3_t                 sway;
	float                  scale;
	float                  forward, right, up;
	float                  bobUp, bobRight;
	float                  recoil;
	float                  reloadTarget;
	float                  reload;    // 0..1 through a reload, -1 when not reloading

	if ( !cg_drawGun.integer ) {
		return;
	}

	/*
	Third person draws the world from outside your head, so a view model would
	be a gun floating in the middle of the screen attached to nothing.
	RF_FIRST_PERSON would not save us here -- it governs portals and mirrors,
	not this -- so the check is explicit.
	*/
	if ( cg_thirdPerson.integer ) {
		return;
	}

	ps = &cg.predictedPlayerState;

	// Dead, spectating or watching the scoreboard at the end: no hands, no gun.
	if ( ps->pm_type == PM_DEAD || ps->pm_type == PM_SPECTATOR
	     || ps->pm_type == PM_INTERMISSION ) {
		return;
	}

	if ( ps->weapon == WP_NONE ) {
		return;
	}

	if ( !cgs.media.weaponModel ) {
		return;    // the model would not load; the warning was printed at load
	}

	w = CF_Weapon( ps->weapon );

	memset( &gun, 0, sizeof( gun ) );

	CG_WeaponSway( sway );
	CG_WeaponBob( ps, &bobUp, &bobRight );

	/*
	Positive `right` means screen-right, which costs a negation later and is
	worth it: cg_gunY is a setting a person types, and a gun that moves left
	when you raise the number is the kind of thing nobody reports and everybody
	works around.

	The sway terms are subtracted because the sway already holds the NEGATIVE of
	how far the view turned. Turn left and sway[YAW] goes negative, so
	subtracting it shifts the gun screen-right -- trailing the turn, which is
	the direction a held object actually lags in.
	*/
	forward = cg_gunX.value;
	right = cg_gunY.value + bobRight - sway[YAW] * CF_GUN_SWAY_SHIFT;
	up = cg_gunZ.value + bobUp - sway[PITCH] * CF_GUN_SWAY_SHIFT;

	/*
	Recoil pushes the gun back and tips it up, on top of what the view is
	already doing. STAT_RECOIL is in hundredths of a degree -- see cf_weapons.h
	on why every weapon number is an integer in fixed units.
	*/
	recoil = (float)ps->stats[STAT_RECOIL] / 100.0f;
	forward -= recoil * CF_GUN_RECOIL_BACK;

	/*
	The reload dip, and the one piece of motion here that is standing in for
	something rather than being the thing itself.

	Driven by a follower toward 0 or 1 rather than by the reload's progress,
	because cgame cannot tell a tactical reload from an empty one after the
	fact: both arrive as a weaponstate and a weaponTime counting down from a
	total nobody transmitted. Easing in and out on a fixed time constant needs
	neither. It no longer stands in for anything: it turns the gun to present
	the magazine well (see CF_GUN_RELOAD_*), and the magazine clip further down
	does the reload itself.
	*/
	reloadTarget = ( ps->weaponstate == WEAPON_RELOADING ) ? 1.0f : 0.0f;
	cg.gunReload = CG_ExpFollow( cg.gunReload, reloadTarget, CF_GUN_FOLLOW_MS );
	up += cg.gunReload * CF_GUN_RELOAD_LIFT;
	right -= cg.gunReload * CF_GUN_RELOAD_IN;

	/*
	Being raised, which unlike the reload IS derivable: weaponTime counts down
	from exactly w->drawTime, so the fraction remaining is how far through the
	draw we are, and the gun rises out of frame on it.
	*/
	if ( ps->weaponstate == WEAPON_RAISING && w->drawTime > 0 ) {
		float frac = (float)ps->weaponTime / (float)w->drawTime;

		if ( frac < 0.0f ) {
			frac = 0.0f;
		} else if ( frac > 1.0f ) {
			frac = 1.0f;
		}
		up -= frac * CF_GUN_RAISE_DROP;
	}

	/*
	POSITION FROM THE VIEW AXIS, ORIENTATION FROM THE ANGLES, and they are
	deliberately not the same basis.

	Offsetting along the gun's own axes instead would mean that every degree of
	sway or recoil also moved the gun sideways and forwards, so a kick would
	shove it across the screen and the two effects would multiply into
	something nobody tuned. The view axis is the stable frame: the gun is
	always half a metre forward and to the right of the eye, and only its
	ORIENTATION wanders.
	*/
	/*
	NOTE THE MINUS ON THE SECOND ONE. viewaxis[1] points LEFT, not right --
	AnglesToAxis builds it as `VectorSubtract( vec3_origin, right, axis[1] )`
	and says so in a one-line comment in q_math.c that is very easy to read
	past. A view model built without the negation sits on the wrong side of the
	screen, which at least announces itself; the same mistake in something less
	visible would not.
	*/
	VectorCopy( cg.refdef.vieworg, gun.origin );
	VectorMA( gun.origin, forward, cg.refdef.viewaxis[0], gun.origin );
	VectorMA( gun.origin, -right, cg.refdef.viewaxis[1], gun.origin );
	VectorMA( gun.origin, up, cg.refdef.viewaxis[2], gun.origin );

	VectorCopy( cg.refdefViewAngles, angles );
	angles[PITCH] += sway[PITCH] - recoil * CF_GUN_RECOIL_PITCH
	               - cg.gunReload * CF_GUN_RELOAD_PITCH;   // negative pitch is nose up
	angles[YAW] += sway[YAW];
	angles[ROLL] += sway[ROLL] + cg.gunReload * CF_GUN_RELOAD_ROLL;

	AnglesToAxis( angles, gun.axis );

	/*
	SCALE LIVES IN THE AXIS because refEntity_t has no scale field. The three
	axis vectors are the model's basis, so scaling all three scales the model,
	and nonNormalizedAxes is the flag that tells the renderer they are no
	longer unit length -- without it, lighting and culling are computed against
	a basis the renderer believes is normalised and the model is lit wrong at
	best.
	*/
	// Directly: the model is in metres and the cvar is units per metre.
	scale = cg_gunScale.value;
	VectorScale( gun.axis[0], scale, gun.axis[0] );
	VectorScale( gun.axis[1], scale, gun.axis[1] );
	VectorScale( gun.axis[2], scale, gun.axis[2] );
	gun.nonNormalizedAxes = qtrue;

	gun.hModel = cgs.media.weaponModel;

	/*
	THE SLIDE.

	Two frames and a blend between them, which is how every animated model in
	this engine is posed -- the renderer interpolates ent.oldframe toward
	ent.frame by (1 - backlerp), so smoothness comes from the blend rather
	than from having authored a lot of frames. Five is plenty for a travel
	this short; see CF_SLIDE_MS.

	THE CLOCK IS THE MUZZLE FLASH. cg.predictedPlayerEntity.muzzleFlashTime is
	stamped by CG_CheckPlayerstateEvents when our own EV_FIRE is predicted, so
	it is already the moment the shot happened, already local, and already
	right on a client that predicted the shot before the server confirmed it.
	Deriving it from weaponTime instead would have been wrong the moment a
	reload started counting the same field down.

	AN EMPTY GUN HOLDS THE SLIDE BACK. ps->ammo[weapon] is what is in the gun
	-- STAT_RESERVE is what is not -- so zero there means the slide locked
	open on the last shot and stays there until a reload closes it. This is
	the one piece of weapon state a player reads without looking at a number,
	and it costs a frame index.
	*/
	{
		int frame = CF_SLIDE_FIRST;
		float lerp = 0.0f;

		if ( ps->ammo[ ps->weapon ] <= 0 && ps->weaponstate != WEAPON_RELOADING ) {
			frame = CF_SLIDE_BACK;
		} else {
			int since = cg.time - cg.predictedPlayerEntity.muzzleFlashTime;

			if ( since >= 0 && since < CF_SLIDE_MS ) {
				/*
				Position along the whole clip, not along the travel: the
				return stroke is frames 2..4 and falls out of the same
				number rather than needing a second case.
				*/
				float t = (float)since / (float)CF_SLIDE_MS * (float)CF_SLIDE_LAST;

				frame = (int)t;
				lerp = t - (float)frame;
				if ( frame >= CF_SLIDE_LAST ) {
					frame = CF_SLIDE_LAST;
					lerp = 0.0f;
				}
			}
		}

		gun.oldframe = frame;
		gun.frame = ( lerp > 0.0f ) ? frame + 1 : frame;
		gun.backlerp = 1.0f - lerp;
	}

	/*
	THE MAGAZINE. For our own gun the progress is read off weaponTime, which
	is exact and predicted: it counts down from the reload's full length, and
	reloadFromEmpty -- stamped by our own predicted EV_RELOAD -- says which
	length that was.
	*/
	reload = -1.0f;
	if ( w && ps->weaponstate == WEAPON_RELOADING ) {
		int total = cg.predictedPlayerEntity.reloadFromEmpty ? w->reloadEmpty : w->reloadTactical;

		if ( total > 0 ) {
			reload = 1.0f - (float)ps->weaponTime / (float)total;
			CG_WeaponReloadFrame( reload, &gun );
		}
	}

	/*
	RF_DEPTHHACK is the one that matters and the one whose absence is baffling.

	Without it the gun is geometry like any other and clips into the wall you
	are standing against -- you walk up to a corner and the muzzle disappears
	into it. DEPTHHACK compresses the view model's depth range so it is drawn
	in front of the world regardless, which is the same lie every shooter
	tells and for the same reason.

	RF_FIRST_PERSON keeps it out of mirrors and portals. RF_MINLIGHT stops it
	going completely black in an unlit room, which matters more here than for
	most models: a player who cannot see their own weapon in a dark corridor
	reads it as the gun having been taken away.
	*/
	gun.renderfx = RF_DEPTHHACK | RF_FIRST_PERSON | RF_MINLIGHT;

	trap_R_AddRefEntityToScene( &gun );

	// Our own shot's flash, at this gun's muzzle and in its depth hack -- our
	// body is not drawn in first person, so this is the only place it can go.
	// cg.predictedPlayerEntity is where our predicted EV_FIRE stamped it.
	CG_AddMuzzleFlash( &cg.predictedPlayerEntity, &gun );

	// And the hands holding it, hung off it -- see CG_AddViewArms.
	{
		const clientInfo_t *ci = &cgs.clientinfo[ps->clientNum & ( MAX_CLIENTS - 1 )];

		CG_AddViewArms( ps->clientNum, &gun, reload, CG_TeamColor( ci->infoValid ? ci->team : TEAM_FREE ) );
	}
}
