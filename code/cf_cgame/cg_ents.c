/*
===========================================================================
catfight -- turning replicated entities into things the renderer can draw.

There is very little here yet because catfight has no art. Other players are
drawn as a plain marker so that a second person in the map is visible at all;
that is scaffolding and goes away as soon as there is a player model.
===========================================================================
*/

#include "cg_local.h"

/*
==================
CG_InterpolateEntityPosition

Entities are drawn between the two snapshots we are holding, so their motion is
smooth even though the server only tells us about them 20 times a second.
==================
*/
static void CG_InterpolateEntityPosition( centity_t *cent ) {
	vec3_t current, next;
	float  f;

	f = cg.frameInterpolation;

	// this will linearly ease between the two snapshot positions -- it is not
	// where the entity "really" is, which is why it is never used for anything
	// that has to agree with the server
	VectorCopy( cent->currentState.pos.trBase, current );
	VectorCopy( cent->nextState.pos.trBase, next );
	cent->lerpOrigin[0] = current[0] + f * ( next[0] - current[0] );
	cent->lerpOrigin[1] = current[1] + f * ( next[1] - current[1] );
	cent->lerpOrigin[2] = current[2] + f * ( next[2] - current[2] );

	VectorCopy( cent->currentState.apos.trBase, current );
	VectorCopy( cent->nextState.apos.trBase, next );
	cent->lerpAngles[0] = LerpAngle( current[0], next[0], f );
	cent->lerpAngles[1] = LerpAngle( current[1], next[1], f );
	cent->lerpAngles[2] = LerpAngle( current[2], next[2], f );
}

/*
===============
CG_CalcEntityLerpPositions
===============
*/
static void CG_CalcEntityLerpPositions( centity_t *cent ) {
	if ( cent->interpolate && cent->currentState.pos.trType == TR_INTERPOLATE ) {
		CG_InterpolateEntityPosition( cent );
		return;
	}

	VectorCopy( cent->currentState.pos.trBase, cent->lerpOrigin );
	VectorCopy( cent->currentState.apos.trBase, cent->lerpAngles );
}

/*
===============
CG_PlayerWeapon

The gun in somebody else's hands.

WHY IT MATTERS MORE THAN IT LOOKS. Until now the only way to know another
player was armed was to be shot by them. A visible weapon is how you read, at
a glance and across a room, that the shape in the doorway is a threat -- and
in an elimination format where a round turns on one exchange, that glance is
most of the information you get.

THERE IS NO HAND TO ATTACH IT TO. The player is a cube with two joints and
neither is a wrist, so the gun is placed at an offset from the body and turned
with the body. That is wrong in the way a placeholder is allowed to be wrong:
it will be attached to a hand bone the day there is one, and the cg_gunHold*
cvars below are the three numbers that get deleted when that happens.

IT ANIMATES, and for free. cent->muzzleFlashTime is stamped on whoever fired
by the same EV_FIRE that stamps our own, so the slide cycles on other people's
guns using the same clock and the same frames as the view model. What it does
NOT do is hold the slide back on an empty gun: that needs their ammo count,
and ammo is in the playerState, which is ours alone.
===============
*/
/*
HELD OUT IN FRONT, measured against the body it has to sit beside.

The marker is the player's bounding box: 32 wide, and -24 to +32 vertically
because the origin sits 24 units above the feet. So the half-width is 16, and
anything closer to the centreline than that is inside the body and not drawn.

FORWARD clears that 16 and puts the gun ahead of the chest, which is where a
pistol is when somebody is pointing it at you -- the pose that matters, since
this only exists to be read across a room. UP is a little above the origin,
which lands around two thirds of the body's height: chest, not head.

These were 26/18/20 for about an hour, tuned against the old 48-unit cube, and
had to move when the marker was corrected to the real hitbox. Worth knowing if
they ever look wrong again: they describe a position relative to the BODY, so
they are only as right as the body is.

CVARS RATHER THAN DEFINES, for the reason cg_gunX/Y/Z are: "does that look
like somebody holding a gun" is a judgement made by looking at it, and the
person looking should not have to rebuild between guesses. That went wrong the
slow way first -- rebuilding, relaunching and hunting a wandering bot around
the map for each attempt, which is a poor way to spend an evening and a worse
way to tune three numbers.

ALL THREE ARE NOW A FALLBACK. A body with a Weapon socket (gen_body.py makes
one in the right hand) gets the gun put there instead, and these only place it
for the sprite and for static models that have no hand.

body is the refEntity the player was drawn with, or NULL for the sprite.
*/

static void CG_PlayerWeapon( centity_t *cent, const refEntity_t *body ) {
	refEntity_t   ent;
	vec3_t        forward, right, up, angles;
	int           since;
	qboolean      inHand;
	orientation_t hand;

	if ( cent->currentState.weapon == WP_NONE ) {
		return;
	}
	// A body on the floor is not holding anything.
	if ( cent->currentState.eFlags & EF_DEAD ) {
		return;
	}
	if ( !cgs.media.weaponModel ) {
		return;
	}

	memset( &ent, 0, sizeof( ent ) );

	/*
	The yaw-only basis for the no-hand fallback's OFFSET. Using the pitched
	basis would swing the gun through an arc around the body as they looked
	up, so a player aiming at the ceiling would hold their pistol above their
	head.
	*/
	VectorCopy( cent->lerpAngles, angles );
	angles[PITCH] = 0;
	angles[ROLL] = 0;
	AngleVectors( angles, forward, right, up );

	/*
	IN THE HAND when there is one -- position AND orientation, from the Weapon
	socket. The torso scrubs torso_aim by view pitch, whose hands point exactly
	where the player looks (gen_body.py proves it on every frame), so the gun
	follows the hand and still points along the view. Mid-reload the torso is
	level and so is the gun, which is what reloading looks like.

	Without a hand -- the sprite, a static model -- the gun is placed by the
	cg_gunHold* offsets and oriented from the view angles, as before.
	*/
	inHand = body && CG_BodySocket( body, "Weapon", &hand );
	if ( inHand ) {
		VectorCopy( hand.origin, ent.origin );
		AxisCopy( hand.axis, ent.axis );
	} else {
		VectorCopy( cent->lerpOrigin, ent.origin );
		VectorMA( ent.origin, cg_gunHoldForward.value, forward, ent.origin );
		VectorMA( ent.origin, cg_gunHoldRight.value, right, ent.origin );
		ent.origin[2] += cg_gunHoldUp.value;

		VectorCopy( cent->lerpAngles, angles );
		angles[ROLL] = 0;
		AnglesToAxis( angles, ent.axis );
	}

	/*
	A SCALE OF ITS OWN, not cg_gunScale.

	cg_gunScale is tuned by eye for how the weapon reads in the corner of your
	own screen and is deliberately not physical -- see cg_weapon.c. Sharing it
	would mean every other player's gun changed size whenever somebody
	adjusted their own view model, which is nonsense.

	This one wants to match the BODY: 56 units to roughly 1.8 metres is about
	31 units per metre, which puts a 204mm pistol at 6.4 units against a
	32-unit-wide torso. Still a cvar, because "does that look like a person
	holding a gun" is a judgement made by looking.
	*/
	{
		float scale = cg_gunWorldScale.value;

		VectorScale( ent.axis[0], scale, ent.axis[0] );
		VectorScale( ent.axis[1], scale, ent.axis[1] );
		VectorScale( ent.axis[2], scale, ent.axis[2] );
		ent.nonNormalizedAxes = qtrue;
	}

	/*
	The socket is where the hand closes, which is the GRIP, not the model's
	origin -- that sits out in front of the trigger guard. Backed off by the
	grip's offset, along the gun's own (scaled) axes, so the grip is what ends
	up in the fist.
	*/
	if ( inHand ) {
		VectorMA( ent.origin, -CF_GUN_GRIP_X, ent.axis[0], ent.origin );
		VectorMA( ent.origin, -CF_GUN_GRIP_Y, ent.axis[1], ent.origin );
		VectorMA( ent.origin, -CF_GUN_GRIP_Z, ent.axis[2], ent.origin );
	}
	VectorCopy( ent.origin, ent.oldorigin );

	ent.hModel = cgs.media.weaponModel;
	ent.renderfx = RF_NOSHADOW;

	// The same cycle the view model plays, off the same stamp. See cg_weapon.c
	// for why the timing is ours and not the animation's.
	since = cg.time - cent->muzzleFlashTime;
	if ( since >= 0 && since < CF_SLIDE_MS ) {
		float t = (float)since / (float)CF_SLIDE_MS * (float)CF_SLIDE_LAST;
		int   frame = (int)t;
		float lerp = t - (float)frame;

		if ( frame >= CF_SLIDE_LAST ) {
			frame = CF_SLIDE_LAST;
			lerp = 0.0f;
		}
		ent.oldframe = frame;
		ent.frame = ( lerp > 0.0f ) ? frame + 1 : frame;
		ent.backlerp = 1.0f - lerp;
	}

	// The magazine coming out, on the same schedule as their off hand.
	CG_WeaponReloadFrame( CG_ReloadProgress( cent ), &ent );

	/*
	WHITE, AND NOT THE TEAM COLOUR.

	Tinting it was the first instinct and it is wrong twice over. It does
	nothing -- cf/w_g17_* have no `rgbGen entity` stage, so shaderRGBA never
	reaches them -- and if a stage were added it would multiply a textured
	diffuse map by a colour under 1.0 and darken art that was authored at the
	brightness it wanted. The body carries the team colour; the gun is a gun.

	Set explicitly rather than left at the memset zero, because zero is black
	and would become visible the moment anybody did add an entity stage.
	*/
	ent.shaderRGBA[0] = 255;
	ent.shaderRGBA[1] = 255;
	ent.shaderRGBA[2] = 255;
	ent.shaderRGBA[3] = 255;

	trap_R_AddRefEntityToScene( &ent );
	CG_AddMuzzleFlash( cent, &ent );
}

/*
===============
CG_Player

A placeholder marker for another player. Deliberately crude -- it is here so
that testing with two clients shows you something, not because it is what a
catfight player is going to look like.
===============
*/
static void CG_Player( centity_t *cent ) {
	refEntity_t   ent;
	clientInfo_t *ci;
	const float  *color;
	qboolean      dead;

	memset( &ent, 0, sizeof( ent ) );

	dead = ( cent->currentState.eFlags & EF_DEAD ) != 0;

	/*
	Colour the marker by side. With no player model this is the only way to tell
	who you are looking at, and in a game decided by elimination that is not a
	nicety -- shooting your own teammate because you could not tell would be the
	single most annoying thing the placeholder could do.
	*/
	ci = &cgs.clientinfo[cent->currentState.clientNum & ( MAX_CLIENTS - 1 )];
	color = CG_TeamColor( ci->infoValid ? ci->team : TEAM_FREE );

	/*
	THE MARKER MUST NOT BE WIDER THAN THE HITBOX IT STANDS FOR.

	radius is a half-extent, so this used to draw a 48-unit-wide sprite for a
	body that is 32 wide -- half again too big. Every shot at the outer third of
	it missed, correctly, and looked like the game eating hits. A placeholder is
	allowed to be crude; it is not allowed to lie about where a player is.

	16 matches CF_PLAYER_WIDTH exactly. Vertically that under-covers the 56-unit
	body, which is the safe direction to be wrong in: it costs a hit nobody
	aimed at rather than claiming one that was never there.
	*/
	/*
	THEIR BODY, the one the server chose for them (G_ClientBody) -- a catgirl
	for a companion, a person for a person. Which model, where its feet go,
	which way it faces and which frame of which animation each half is on are
	all CG_AddPlayerBody's, in cg_players.c.

	Falls through to the sprite when the body will not load, because a player
	who is invisible is far worse than a player who is a coloured square -- and
	a missing or misnamed model would otherwise empty the map.
	*/
	if ( CG_AddPlayerBody( cent, color, &ent ) ) {
		CG_PlayerWeapon( cent, &ent );
		return;
	}

	ent.reType = RT_SPRITE;
	ent.customShader = cgs.media.placeholder;
	ent.radius = dead ? 12 : CF_PLAYER_WIDTH / 2;
	ent.renderfx = RF_NOSHADOW;
	ent.shaderRGBA[0] = (byte)( color[0] * ( dead ? 100 : 255 ) );
	ent.shaderRGBA[1] = (byte)( color[1] * ( dead ? 100 : 255 ) );
	ent.shaderRGBA[2] = (byte)( color[2] * ( dead ? 100 : 255 ) );
	ent.shaderRGBA[3] = 255;

	// Centred on the middle of the bounding box, so aiming at the middle of the
	// marker is aiming at the middle of the player.
	VectorCopy( cent->lerpOrigin, ent.origin );
	ent.origin[2] += dead ? -12 : ( CF_MINS_Z + CF_MAXS_Z ) / 2;

	trap_R_AddRefEntityToScene( &ent );

	// Also on the sprite path: whether the body is a cube or a billboard, a
	// player holding a weapon should be visibly holding one.
	CG_PlayerWeapon( cent, NULL );
}

/*
===============
CG_AddMuzzleFlash

The flash of a shot, at the muzzle of the gun that fired it: the flame and
star of models/weapons/flash.iqm, and the light it throws on the room.

AT THE GUN, in the gun's own frame. `gun` is the weapon exactly as it is being
drawn this frame -- the view model for our own shots, the one in a player's
hand for everybody else's -- so the flash inherits its position, its aim, its
scale and its render flags (depth hack and all in first person) and cannot
come apart from it. The muzzle point is measured off the barrel mesh
(CF_GUN_MUZZLE_*).

THE LIGHT IS THE HALF THAT MATTERS AT A DISTANCE: it tells you a shot was fired
over there, from behind that corner, without you seeing the shooter. The flame
is the half that matters up close. Both fade across CF_MUZZLE_FLASH_TIME rather
than switching off, so a quick pair of shots reads as a flicker and not a
strobe.

NOT THE SAME PICTURE TWICE. Each shot rolls the flash about the barrel and
scales it a little, seeded from the time of that shot -- so it is random from
shot to shot and steady across the three frames one flash lasts.
===============
*/
void CG_AddMuzzleFlash( const centity_t *cent, const refEntity_t *gun ) {
	refEntity_t flash;
	vec3_t      muzzle, angles;
	float       age, fade, roll, size;
	int         seed, i;
	vec3_t      spun[3];

	if ( !cent->muzzleFlashTime ) {
		return;
	}
	age = (float)( cg.time - cent->muzzleFlashTime );
	if ( age < 0 || age >= CF_MUZZLE_FLASH_TIME ) {
		return;
	}
	fade = 1.0f - age / (float)CF_MUZZLE_FLASH_TIME;

	// The muzzle, along the gun's own (scaled) axes -- the same arithmetic that
	// puts the grip in the hand.
	VectorCopy( gun->origin, muzzle );
	VectorMA( muzzle, CF_GUN_MUZZLE_X, gun->axis[0], muzzle );
	VectorMA( muzzle, CF_GUN_MUZZLE_Y, gun->axis[1], muzzle );
	VectorMA( muzzle, CF_GUN_MUZZLE_Z, gun->axis[2], muzzle );

	// Warm, and slightly over-bright in red -- a muzzle flash is not a torch.
	trap_R_AddLightToScene( muzzle, 220.0f * fade, 1.0f, 0.85f, 0.55f );

	if ( !cgs.media.flashModel ) {
		return;
	}

	seed = cent->muzzleFlashTime;
	roll = (float)( Q_rand( &seed ) % 360 );
	size = 0.85f + 0.3f * (float)( Q_rand( &seed ) % 1000 ) / 1000.0f;

	/*
	Bigger still in first person. The camera sits behind the gun, so the
	slide hides the muzzle and most of the flame; drawn at the same size as
	everybody else's, our own shot read as a faint glow behind the gun.
	*/
	if ( gun->renderfx & RF_FIRST_PERSON ) {
		size *= 1.5f;
	}

	/*
	Rolled about the barrel: the gun's forward axis stays put, its left and up
	turn together. Done on the gun's own axes, so the roll is about the barrel
	whichever way the gun happens to point.
	*/
	VectorSet( angles, 0, 0, roll );
	{
		vec3_t r[3];

		AnglesToAxis( angles, r );
		for ( i = 0; i < 3; i++ ) {
			VectorScale( gun->axis[0], r[i][0], spun[i] );
			VectorMA( spun[i], r[i][1], gun->axis[1], spun[i] );
			VectorMA( spun[i], r[i][2], gun->axis[2], spun[i] );
		}
	}

	memset( &flash, 0, sizeof( flash ) );
	flash.reType = RT_MODEL;
	flash.hModel = cgs.media.flashModel;
	flash.renderfx = gun->renderfx | RF_NOSHADOW;
	VectorCopy( muzzle, flash.origin );
	VectorCopy( muzzle, flash.oldorigin );
	for ( i = 0; i < 3; i++ ) {
		VectorScale( spun[i], size, flash.axis[i] );
	}
	flash.nonNormalizedAxes = qtrue;

	// The fade lives in the colour: the shaders are additive, which never
	// reads alpha, and dimming an additive surface is fading it.
	flash.shaderRGBA[0] = (byte)( 255 * fade );
	flash.shaderRGBA[1] = (byte)( 255 * fade );
	flash.shaderRGBA[2] = (byte)( 255 * fade );
	flash.shaderRGBA[3] = 255;

	trap_R_AddRefEntityToScene( &flash );
}

/*
===============
CG_AddCEntity
===============
*/
static void CG_AddCEntity( centity_t *cent ) {
	CG_CalcEntityLerpPositions( cent );

	// anything the entity is announcing happens before it is drawn, so an effect
	// can be spawned at the position it is being drawn at this frame. The muzzle
	// flash is one of those: it is drawn from CG_PlayerWeapon, at the gun.
	CG_CheckEvents( cent );

	// an entity whose eType is an event has no body to draw
	if ( cent->currentState.eType >= ET_EVENTS ) {
		return;
	}

	switch ( cent->currentState.eType ) {
	case ET_PLAYER:
		CG_Player( cent );
		break;

	default:
		// nothing else has a representation yet
		break;
	}
}

/*
===============
CG_AddOwnBody

Our own body, for the death camera to look at.

The server NEVER SENDS a client its own entity -- sv_snapshot.c rebuilds it
from the playerstate instead -- so there is nothing in the snapshot to draw.
It is built here the same way the server builds everybody else's
(CF_PlayerStateToEntityState), from a COPY of the predicted playerstate
because that conversion also consumes pending events, which must stay ours to
fire. The event field is cleared for the same reason, and the body is drawn
through CG_Player alone so nothing here runs CG_CheckEvents on it.
===============
*/
void CG_AddOwnBody( void ) {
	centity_t    *cent = &cg.predictedPlayerEntity;
	playerState_t ps = cg.predictedPlayerState;

	CF_PlayerStateToEntityState( &ps, &cent->currentState, qfalse );
	cent->currentState.event = 0;
	cent->currentState.eventParm = 0;

	VectorCopy( ps.origin, cent->lerpOrigin );
	VectorCopy( ps.viewangles, cent->lerpAngles );

	CG_Player( cent );
}

/*
===============
CG_AddPacketEntities
===============
*/
void CG_AddPacketEntities( void ) {
	int        num;
	centity_t *cent;

	for ( num = 0; num < cg.snap->numEntities; num++ ) {
		cent = &cg_entities[cg.snap->entities[num].number];

		// our own body is not drawn in first person
		if ( cent->currentState.number == cg.snap->ps.clientNum && !cg_thirdPerson.integer ) {
			continue;
		}

		CG_AddCEntity( cent );
	}
}




