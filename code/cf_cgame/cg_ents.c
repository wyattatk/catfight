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

	ent.reType = RT_SPRITE;
	ent.customShader = cgs.media.placeholder;
	ent.radius = dead ? 12 : 24;
	ent.renderfx = RF_NOSHADOW;
	ent.shaderRGBA[0] = (byte)( color[0] * ( dead ? 100 : 255 ) );
	ent.shaderRGBA[1] = (byte)( color[1] * ( dead ? 100 : 255 ) );
	ent.shaderRGBA[2] = (byte)( color[2] * ( dead ? 100 : 255 ) );
	ent.shaderRGBA[3] = 255;

	VectorCopy( cent->lerpOrigin, ent.origin );
	ent.origin[2] += dead ? -12 : 8;

	trap_R_AddRefEntityToScene( &ent );
}

/*
===============
CG_AddCEntity
===============
*/
static void CG_AddCEntity( centity_t *cent ) {
	CG_CalcEntityLerpPositions( cent );

	// anything the entity is announcing happens before it is drawn, so an effect
	// can be spawned at the position it is being drawn at this frame
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
