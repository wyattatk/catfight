/*
===========================================================================
catfight -- player bodies: which model, which animation, which frame.

Every body shares one skeleton (see mapping/gen_body.py), so everything here is
written against animation NAMES, never against a particular model's frame
numbers. A body brings its own animation.cfg saying where each named clip sits
in its IQM, which is what lets a Mixamo run of 26 frames and a hand-keyed one
of 20 drive the same code.

TWO HALVES. A body that ships lower.skin and upper.skin is drawn twice: once
as legs, playing whatever the feet are doing, and once as a torso, playing
what the hands are doing -- so somebody can reload while they run. The renderer
has no notion of this; it is the same model drawn with two frames and two
skins, and the torso copy is moved so that its Spine joint lands exactly where
the legs copy put Spine. A body without the skins is drawn once, whole.

None of this touches the hitbox. What is drawn here is decided entirely on the
client from state that was already replicated; the box that bullets hit is
CF_PlayerBounds, the same for every body, and no cosmetic reaches it.
===========================================================================
*/

#include "cg_local.h"

static const char *cfAnimNames[CF_ANIM_COUNT] = {
	"idle",
	"run",
	"crouch",
	"crouchwalk",
	"jump",
	"death",
	"torso_idle",
	"torso_reload",
	"torso_aim",
	"fp_idle",
	"fp_reload",
};

typedef struct {
	int      first;
	int      count;     // 0: this body does not have the clip
	float    fps;
	qboolean loop;
} cfAnim_t;

/*
THE BODIES, each loaded once and kept by name.

Several at once now -- a companion is a catgirl and the person she fights
beside is not -- so a body is looked up by the name the server gave each
player rather than there being one body everybody shares. Loaded when a player
with that body first appears (CG_RegisterBody, from CG_NewClientInfo), so the
cost lands on a player joining rather than on the frame they come round a
corner. A name that will not load is remembered as not loading, so it is tried
once and not every frame.

A handful of slots is plenty: every body the game has fits with room over, and
a cosmetic economy that outgrows it will want this to be something else anyway.
*/
#define MAX_BODIES  16

typedef struct {
	char      name[32];
	qhandle_t model;       // 0: tried, would not load
	qhandle_t lowerSkin;   // both or neither; see CG_BodySplits
	qhandle_t upperSkin;
	qhandle_t fpSkin;      // the arms alone, for first person
	cfAnim_t  anims[CF_ANIM_COUNT];
} cfBody_t;

static cfBody_t bodies[MAX_BODIES];
static int      numBodies;

/*
A body name is a directory name and nothing else. It arrives in a configstring
the server wrote, but it is about to become a file path, so anything that could
walk out of models/players/ is refused rather than trusted.
*/
static qboolean CG_ValidBodyName( const char *name ) {
	if ( !name[0] ) {
		return qfalse;
	}
	for ( ; *name; name++ ) {
		if ( !( ( *name >= 'a' && *name <= 'z' ) || ( *name >= '0' && *name <= '9' ) || *name == '_' ) ) {
			return qfalse;
		}
	}
	return qtrue;
}

// A file in a body's own directory.
static void CG_BodyFile( const cfBody_t *b, const char *file, char *out, int outSize ) {
	Com_sprintf( out, outSize, "models/players/%s/%s", b->name, file );
}

/*
Read animation.cfg from the model's own directory: one clip per line,

	name  first  count  fps  loop

Names this code does not know are skipped, so a body can carry clips nothing
plays yet. A body with no animation.cfg at all is not an error -- it is a
static model, like the cube, and draws frame 0 forever.
*/
static void CG_LoadAnimations( cfBody_t *b ) {
	char         path[MAX_QPATH];
	char         text[4096];
	char        *p, *tok;
	fileHandle_t f;
	int          len, i;

	memset( b->anims, 0, sizeof( b->anims ) );

	CG_BodyFile( b, "animation.cfg", path, sizeof( path ) );

	len = trap_FS_FOpenFile( path, &f, FS_READ );
	if ( len <= 0 ) {
		if ( f ) {
			trap_FS_FCloseFile( f );
		}
		return;
	}
	if ( len >= (int)sizeof( text ) ) {
		trap_FS_FCloseFile( f );
		Com_Printf( S_COLOR_YELLOW "%s is too long, ignoring it\n", path );
		return;
	}
	trap_FS_Read( text, len, f );
	text[len] = '\0';
	trap_FS_FCloseFile( f );

	p = text;
	for ( ;; ) {
		cfAnim_t anim;
		int      which = -1;

		tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;
		}
		for ( i = 0; i < CF_ANIM_COUNT; i++ ) {
			if ( !Q_stricmp( tok, cfAnimNames[i] ) ) {
				which = i;
			}
		}
		anim.first = atoi( COM_Parse( &p ) );
		anim.count = atoi( COM_Parse( &p ) );
		anim.fps = atof( COM_Parse( &p ) );
		anim.loop = atoi( COM_Parse( &p ) ) != 0;

		if ( which >= 0 && anim.count > 0 ) {
			if ( anim.fps <= 0 ) {
				anim.fps = 30;
			}
			b->anims[which] = anim;
		}
	}
}

/*
The skins that hide one half each. Asked for by path rather than probed for
first: trap_R_RegisterSkin of a file that is not there returns 0, which is
exactly "this body is not split", so a missing pair needs no special case.
*/
static qhandle_t CG_LoadSkin( const cfBody_t *b, const char *file ) {
	char path[MAX_QPATH];

	CG_BodyFile( b, file, path, sizeof( path ) );
	return trap_FS_FOpenFile( path, NULL, FS_READ ) > 0 ? trap_R_RegisterSkin( path ) : 0;
}

/*
A body by name: models/players/<name>/<name>.iqm and the files beside it.
NULL when the name is not one, or the model would not load.
*/
static const cfBody_t *CG_FindBody( const char *name ) {
	char      path[MAX_QPATH];
	cfBody_t *b;
	int       i;

	if ( !CG_ValidBodyName( name ) ) {
		return NULL;
	}
	for ( i = 0; i < numBodies; i++ ) {
		if ( !strcmp( bodies[i].name, name ) ) {
			return bodies[i].model ? &bodies[i] : NULL;
		}
	}
	if ( numBodies == MAX_BODIES ) {
		return NULL;
	}

	b = &bodies[numBodies++];
	memset( b, 0, sizeof( *b ) );
	Q_strncpyz( b->name, name, sizeof( b->name ) );
	Com_sprintf( path, sizeof( path ), "models/players/%s/%s.iqm", name, name );
	b->model = trap_R_RegisterModel( path );
	if ( !b->model ) {
		Com_Printf( S_COLOR_YELLOW "body '%s' would not load (%s); drawn as a marker\n", name, path );
		return NULL;
	}
	CG_LoadAnimations( b );
	b->lowerSkin = CG_LoadSkin( b, "lower.skin" );
	b->upperSkin = CG_LoadSkin( b, "upper.skin" );
	b->fpSkin = CG_LoadSkin( b, "fp.skin" );
	return b;
}

void CG_RegisterBody( const char *name ) {
	CG_FindBody( name );
}

void CG_ClearBodies( void ) {
	memset( bodies, 0, sizeof( bodies ) );
	numBodies = 0;
}

/*
The body a player is drawn with: the one the server chose for them, unless
cg_forceBody is making everybody the same for a test.
*/
static const cfBody_t *CG_ClientBody( int clientNum ) {
	const clientInfo_t *ci = &cgs.clientinfo[clientNum & ( MAX_CLIENTS - 1 )];

	if ( cg_forceBody.string[0] ) {
		return CG_FindBody( cg_forceBody.string );
	}
	return ci->infoValid ? CG_FindBody( ci->body ) : NULL;
}

// Whether this body can be drawn as two halves: both skins, and an upper-body
// clip for the torso to play.
static qboolean CG_BodySplits( const cfBody_t *b ) {
	return b->lowerSkin && b->upperSkin && b->anims[CF_ANIM_TORSO_IDLE].count;
}

/*
WHAT THEIR LEGS ARE DOING, from the entity state alone.

Everything used here was already being sent: velocity rides in pos.trDelta,
groundEntityNum says whether they are in the air, and EF_DUCKED / EF_DEAD are
bits in eFlags. No new network traffic buys any of this.
*/
static int CG_ChooseLegsAnim( const centity_t *cent, float speed ) {
	const entityState_t *s = &cent->currentState;

	if ( s->eFlags & EF_DEAD ) {
		return CF_ANIM_DEATH;
	}
	if ( s->groundEntityNum == ENTITYNUM_NONE ) {
		return CF_ANIM_JUMP;
	}
	if ( s->eFlags & EF_DUCKED ) {
		return speed > CF_ANIM_MOVING_SPEED ? CF_ANIM_CROUCHWALK : CF_ANIM_CROUCH;
	}
	return speed > CF_ANIM_MOVING_SPEED ? CF_ANIM_RUN : CF_ANIM_IDLE;
}

// The clip to actually play: the one asked for, or the nearest thing this body
// has. Every body has to have idle; everything falls back to it.
static int CG_ResolveAnim( const cfBody_t *b, int anim ) {
	if ( b->anims[anim].count ) {
		return anim;
	}
	if ( anim == CF_ANIM_CROUCHWALK && b->anims[CF_ANIM_CROUCH].count ) {
		return CF_ANIM_CROUCH;
	}
	if ( anim == CF_ANIM_TORSO_RELOAD ) {
		return CF_ANIM_TORSO_IDLE;
	}
	return CF_ANIM_IDLE;
}

/*
How far through a reload somebody is, 0..1, or -1 if they are not reloading.

Timed from the EV_RELOAD that started it, against the weapon table's own
reload time for whichever kind it was -- the event says which. The same
number drives the torso here and the magazine in the gun (CG_WeaponReloadFrame),
so the hand and the magazine cannot drift apart.
*/
float CG_ReloadProgress( const centity_t *cent ) {
	float u;

	if ( !cent->reloadTime || cent->reloadTotal <= 0 ) {
		return -1.0f;
	}
	u = (float)( cg.time - cent->reloadTime ) / (float)cent->reloadTotal;
	if ( u < 0.0f || u >= 1.0f ) {
		return -1.0f;
	}
	return u;
}

/*
Step a clip along and write the frame pair into ent.

phase is in frames and belongs to the caller, per entity and per half. A
negative rate plays a looping clip backwards.
*/
static void CG_RunAnim( const cfAnim_t *anim, float *phase, float rate, refEntity_t *ent ) {
	int frame, next;

	*phase += rate * cg.frametime * 0.001f;

	if ( anim->loop ) {
		*phase = fmodf( *phase, (float)anim->count );
		if ( *phase < 0 ) {
			*phase += anim->count;
		}
		frame = (int)*phase;
		next = ( frame + 1 ) % anim->count;
	} else {
		if ( *phase > anim->count - 1 ) {
			*phase = anim->count - 1;
		}
		if ( *phase < 0 ) {
			*phase = 0;
		}
		frame = (int)*phase;
		next = frame + 1 < anim->count ? frame + 1 : frame;
	}

	ent->oldframe = anim->first + frame;
	ent->frame = anim->first + next;
	ent->backlerp = 1.0f - ( *phase - (float)frame );
}

/*
THE LEGS, or the whole body when it is not split. Fills in position, facing
and frame.
*/
static void CG_LegsFrame( const cfBody_t *b, centity_t *cent, refEntity_t *ent ) {
	const cfAnim_t *anim;
	vec3_t          angles, forward, vel;
	float           speed, rate;
	int             want;

	VectorSet( angles, 0, cent->lerpAngles[YAW], 0 );

	VectorCopy( cent->currentState.pos.trDelta, vel );
	vel[2] = 0;
	speed = VectorLength( vel );

	want = CG_ResolveAnim( b, CG_ChooseLegsAnim( cent, speed ) );
	anim = &b->anims[want];
	if ( !anim->count ) {
		// A static model: frame 0, nothing to advance.
		ent->frame = ent->oldframe = 0;
		ent->backlerp = 0;
		return;
	}

	// Every clip starts from its first frame. Carrying the phase over would
	// start a death halfway through the fall.
	if ( want != cent->anim ) {
		cent->anim = want;
		cent->animPhase = 0;
	}

	/*
	DEATH RUNS OFF THE CLOCK IT HAPPENED AT. s.time is stamped by the server at
	the moment of death and carried over to the corpse, so a body that comes
	into view already dead -- or the corpse that takes over from the player a
	moment later -- is drawn lying down rather than falling over again.
	*/
	if ( want == CF_ANIM_DEATH && cent->currentState.time ) {
		cent->animPhase = (float)( cg.time - cent->currentState.time ) * 0.001f * anim->fps;
		CG_RunAnim( anim, &cent->animPhase, 0, ent );
		return;
	}

	/*
	LEGS MATCH THE GROUND. The run is played at a rate set by how fast they
	are actually going, so a strafe-walker takes slow steps and a sprinter fast
	ones, and it runs backwards when they are backpedalling -- which is not
	right, but it is much less wrong than moonwalking forwards. The other clips
	play at their own authored rate.
	*/
	rate = anim->fps;
	if ( want == CF_ANIM_RUN || want == CF_ANIM_CROUCHWALK ) {
		float stride = cg_animStride.value > 1 ? cg_animStride.value : 1;

		rate = anim->count * speed / stride;
		AngleVectors( angles, forward, NULL, NULL );
		if ( DotProduct( forward, vel ) < 0 ) {
			rate = -rate;
		}
	}
	CG_RunAnim( anim, &cent->animPhase, rate, ent );
}

/*
THE TORSO: reloading, or aiming where they look.

Aiming is torso_aim scrubbed by view pitch rather than played: the frame IS the
angle, so the arms and the gun in them follow the view exactly and at once.
Reloading is authored level and wins while it lasts -- a reload is a moment,
and a player reloading is not aiming anywhere in particular. A body without
torso_aim falls back to torso_idle, which is the level hold.
*/
static void CG_TorsoFrame( const cfBody_t *b, centity_t *cent, refEntity_t *ent ) {
	const cfAnim_t *anim;
	float           reload = CG_ReloadProgress( cent );
	int             want;

	if ( reload >= 0 ) {
		want = CG_ResolveAnim( b, CF_ANIM_TORSO_RELOAD );
	} else if ( b->anims[CF_ANIM_TORSO_AIM].count > 1 ) {
		float pitch = AngleNormalize180( cent->lerpAngles[PITCH] );
		float t;

		anim = &b->anims[CF_ANIM_TORSO_AIM];
		cent->torsoAnim = CF_ANIM_TORSO_AIM;

		// Quake's pitch is positive DOWN, and frame 0 is looking up.
		t = ( pitch + CF_AIM_PITCH ) / ( 2.0f * CF_AIM_PITCH );
		if ( t < 0.0f ) {
			t = 0.0f;
		} else if ( t > 1.0f ) {
			t = 1.0f;
		}
		cent->torsoPhase = t * (float)( anim->count - 1 );
		CG_RunAnim( anim, &cent->torsoPhase, 0, ent );
		return;
	} else {
		want = CG_ResolveAnim( b, CF_ANIM_TORSO_IDLE );
	}
	anim = &b->anims[want];

	if ( want != cent->torsoAnim ) {
		cent->torsoAnim = want;
		cent->torsoPhase = 0;
	}

	// The reload is placed by how far through it they are, not stepped at a
	// frame rate, for the reason given at CG_ReloadProgress.
	if ( want == CF_ANIM_TORSO_RELOAD ) {
		cent->torsoPhase = reload * (float)( anim->count - 1 );
		CG_RunAnim( anim, &cent->torsoPhase, 0, ent );
		return;
	}
	CG_RunAnim( anim, &cent->torsoPhase, anim->fps, ent );
}

/*
===============
CG_FlinchFromImpact

A round landed in somebody at `impact`; make their body flinch away from it.

FROM THE IMPACT EVENT, not from EV_PAIN. Pain is sent to the victim's entity
but carries only the damage, and is debounced; EV_BULLET_FLESH reaches every
client with the exact point the round hit and who fired it, which is
everything a flinch needs -- a body, and a direction. No new network traffic.

The victim is whichever living player body the point lies in (their box, with
a little slack for the difference between where we draw them and where the
server traced them). Our own body is not drawn in first person, so it is
skipped.
===============
*/
#define CF_FLINCH_MS    260
#define CF_FLINCH_DEG   18.0f   // 14 was there but hard to read across a room
#define CF_FLINCH_SLACK 8.0f

void CG_FlinchFromImpact( int shooter, const vec3_t impact ) {
	centity_t *best = NULL;
	float      bestDist = 1e9f;
	vec3_t     from, dir;
	int        i;

	for ( i = 0; i < cg.snap->numEntities; i++ ) {
		centity_t *cent = &cg_entities[cg.snap->entities[i].number];
		vec3_t     d;
		float      dist;

		if ( cent->currentState.eType != ET_PLAYER || ( cent->currentState.eFlags & EF_DEAD ) ) {
			continue;
		}
		if ( cent->currentState.number == cg.snap->ps.clientNum ) {
			continue;
		}
		VectorSubtract( impact, cent->lerpOrigin, d );
		if ( fabs( d[0] ) > CF_PLAYER_WIDTH / 2 + CF_FLINCH_SLACK
		     || fabs( d[1] ) > CF_PLAYER_WIDTH / 2 + CF_FLINCH_SLACK
		     || d[2] < CF_MINS_Z - CF_FLINCH_SLACK || d[2] > CF_MAXS_Z + CF_FLINCH_SLACK ) {
			continue;
		}
		dist = VectorLength( d );
		if ( dist < bestDist ) {
			bestDist = dist;
			best = cent;
		}
	}
	if ( !best ) {
		return;
	}

	// Which way the round was travelling, flattened: the flinch is a lean, and
	// a shot from above should not fold anybody in half.
	if ( shooter == cg.snap->ps.clientNum ) {
		VectorCopy( cg.predictedPlayerState.origin, from );
	} else if ( shooter >= 0 && shooter < MAX_CLIENTS ) {
		VectorCopy( cg_entities[shooter].lerpOrigin, from );
	} else {
		return;
	}
	VectorSubtract( best->lerpOrigin, from, dir );
	dir[2] = 0;
	if ( VectorNormalize( dir ) < 1.0f ) {
		return;
	}

	VectorCopy( dir, best->flinchDir );
	best->flinchTime = cg.time ? cg.time : 1;
}

/*
How far into a flinch a body is, as a fraction of CF_FLINCH_DEG: a snap out in
the first fifth, then an easing back. 0 when there is none.
*/
static float CG_FlinchAmount( const centity_t *cent ) {
	float t;

	if ( !cent->flinchTime ) {
		return 0.0f;
	}
	t = (float)( cg.time - cent->flinchTime ) / (float)CF_FLINCH_MS;
	if ( t < 0.0f || t >= 1.0f ) {
		return 0.0f;
	}
	if ( t < 0.2f ) {
		return t / 0.2f;
	}
	t = ( t - 0.2f ) / 0.8f;
	return ( 1.0f - t ) * ( 1.0f - t );
}

/*
Transforms as orientation_t: a point p goes to origin + sum p[i] * axis[i].
Only ever rotations here, so the inverse is the transpose.
*/
static void CG_OrientCompose( const orientation_t *a, const orientation_t *b, orientation_t *out ) {
	orientation_t r;
	int           i, k;

	VectorCopy( a->origin, r.origin );
	for ( i = 0; i < 3; i++ ) {
		VectorMA( r.origin, b->origin[i], a->axis[i], r.origin );
	}
	for ( k = 0; k < 3; k++ ) {
		VectorClear( r.axis[k] );
		for ( i = 0; i < 3; i++ ) {
			VectorMA( r.axis[k], b->axis[k][i], a->axis[i], r.axis[k] );
		}
	}
	*out = r;
}

// Built in a local and copied out, so inverting in place (out == a) works:
// writing the transposed axes straight into *out would overwrite the entries
// still to be read. That exact mistake once put the torso on the floor.
static void CG_OrientInvert( const orientation_t *a, orientation_t *out ) {
	orientation_t r;
	int           i, k;

	for ( k = 0; k < 3; k++ ) {
		for ( i = 0; i < 3; i++ ) {
			r.axis[k][i] = a->axis[i][k];
		}
	}
	for ( i = 0; i < 3; i++ ) {
		r.origin[i] = -DotProduct( a->origin, a->axis[i] );
	}
	*out = r;
}

// Where a joint is on a drawn entity, in the model's own space, blended the
// same way the renderer blends the frames it is drawn with.
static qboolean CG_JointOnModel( const refEntity_t *ent, const char *name, orientation_t *out ) {
	return trap_R_LerpTag( out, ent->hModel, ent->oldframe, ent->frame,
	                       1.0f - ent->backlerp, name ) != 0;
}

/*
===============
CG_AddPlayerBody

Draws another player's body. Returns qfalse when there is no body model to
draw, and the caller falls back to the sprite. On success *hands is the copy
that carries the arms, for the weapon to be put in.
===============
*/
qboolean CG_AddPlayerBody( centity_t *cent, const float *color, refEntity_t *hands ) {
	refEntity_t     legs, torso;
	orientation_t   legsSpine, torsoSpine, place;
	vec3_t          angles, spine;
	float           flinch;
	const cfBody_t *b;

	// Whose body: a corpse carries the clientNum of whoever it was.
	b = CG_ClientBody( cent->currentState.clientNum );
	if ( !b ) {
		return qfalse;
	}

	memset( &legs, 0, sizeof( legs ) );
	legs.hModel = b->model;
	legs.reType = RT_MODEL;
	legs.renderfx = RF_NOSHADOW;
	legs.shaderRGBA[0] = (byte)( color[0] * 255 );
	legs.shaderRGBA[1] = (byte)( color[1] * 255 );
	legs.shaderRGBA[2] = (byte)( color[2] * 255 );
	legs.shaderRGBA[3] = 255;

	/*
	FEET ON THE FLOOR. A body model has its origin at its feet, the way every
	modelling tool and Mixamo author one; the entity's origin is CF_MINS_Z
	above them.
	*/
	VectorCopy( cent->lerpOrigin, legs.origin );
	legs.origin[2] += CF_MINS_Z;
	VectorCopy( legs.origin, legs.oldorigin );

	/*
	YAW ONLY. lerpAngles is the full view, pitch included, and a body built
	from it tips over backwards when its owner looks at the ceiling. Where they
	are looking up and down is the gun's business; see CG_PlayerWeapon.
	*/
	VectorSet( angles, 0, cent->lerpAngles[YAW], 0 );
	AnglesToAxis( angles, legs.axis );

	CG_LegsFrame( b, cent, &legs );

	// Dead, or a body that does not come in halves: one copy, all of it.
	if ( ( cent->currentState.eFlags & EF_DEAD ) || !CG_BodySplits( b ) ) {
		trap_R_AddRefEntityToScene( &legs );
		*hands = legs;
		return qtrue;
	}

	torso = legs;
	legs.customSkin = b->lowerSkin;
	torso.customSkin = b->upperSkin;
	CG_TorsoFrame( b, cent, &torso );

	/*
	PIN THE TORSO ONTO THE LEGS AT SPINE.

	Both copies are the same skeleton at different frames. The legs copy's
	Spine is where the hips, the crouch and the lean have put it; the torso
	copy's Spine is wherever its own clip left it. Moving the whole torso copy
	by  legs * legsSpine * inverse(torsoSpine)  lands its Spine on the legs'
	Spine exactly -- position and rotation -- and everything above follows,
	still posed by the torso clip. Hips and Spine themselves are drawn by the
	legs copy, which is why the split in gen_body.py is at Spine1.

	If either lookup fails the model has no Spine and cannot be split; drawing
	both halves unpinned would tear the body in two, so it is drawn whole.
	*/
	if ( !CG_JointOnModel( &legs, "Spine", &legsSpine )
	     || !CG_JointOnModel( &torso, "Spine", &torsoSpine ) ) {
		legs.customSkin = 0;
		trap_R_AddRefEntityToScene( &legs );
		*hands = legs;
		return qtrue;
	}
	VectorCopy( legs.origin, place.origin );
	AxisCopy( legs.axis, place.axis );
	CG_OrientCompose( &place, &legsSpine, &place );
	VectorCopy( place.origin, spine );     // where the torso pivots, in the world
	CG_OrientInvert( &torsoSpine, &torsoSpine );
	CG_OrientCompose( &place, &torsoSpine, &place );

	/*
	THE FLINCH: the whole torso copy tipped about the spine joint, the top
	going the way the round was travelling. Head, arms and the gun in the hand
	all go with it, because they are all on this copy -- which is the point of
	doing it here rather than in an animation.
	*/
	flinch = CG_FlinchAmount( cent );
	if ( flinch > 0.0f ) {
		vec3_t up = { 0, 0, 1 }, pivotAxis, rel;
		float  deg = CF_FLINCH_DEG * flinch;
		int    i;

		CrossProduct( up, cent->flinchDir, pivotAxis );
		VectorSubtract( place.origin, spine, rel );
		RotatePointAroundVector( place.origin, pivotAxis, rel, deg );
		VectorAdd( place.origin, spine, place.origin );
		for ( i = 0; i < 3; i++ ) {
			vec3_t a;

			VectorCopy( place.axis[i], a );
			RotatePointAroundVector( place.axis[i], pivotAxis, a, deg );
		}
	}

	VectorCopy( place.origin, torso.origin );
	VectorCopy( place.origin, torso.oldorigin );
	AxisCopy( place.axis, torso.axis );

	trap_R_AddRefEntityToScene( &legs );
	trap_R_AddRefEntityToScene( &torso );
	*hands = torso;
	return qtrue;
}

/*
===============
CG_AddViewArms

Your own arms in first person, holding the view-model gun. Returns qfalse,
drawing nothing, for a body without first-person arms; the gun alone is what
was drawn before this existed and is still right on its own.

HUNG OFF THE GUN, not the camera. The gun already carries every bit of
first-person motion -- sway, bob, recoil, the reload tilt that shows the
magazine well -- tuned by eye over a long time, so rather than build all of it
again for the arms, the arms are placed wherever puts their Weapon socket on
the gun's grip, lined up with the gun's own axes. The socket was built so that
is exactly how a gripping hand holds it (WEAPON_SOCKET in gen_body.py), and
the first-person clips were posed so that, hung there, the shoulders land at
the camera.

SCALED WITH THE GUN. The view-model gun is drawn at cg_gunScale units per
metre, bigger than life, the way every shooter draws its own weapon; body
models are CF_BODY_UNITS_PER_METRE. Scaled by the ratio, the hands grow with
the gun and still fit it.

`reload` is how far through a reload we are, 0..1, or -1: the same number that
places the magazine (CG_WeaponReloadFrame), so the hand pulling it out and the
magazine coming out are one clock.
===============
*/
qboolean CG_AddViewArms( int clientNum, const refEntity_t *gun, float reload, const float *color ) {
	refEntity_t     arms;
	const cfAnim_t *anim;
	orientation_t   socket, grip, place;
	float           gunScale, k, phase;
	int             i;
	const cfBody_t *b;

	// Your own arms are your own body's: a person sees a person's hands.
	b = CG_ClientBody( clientNum );
	if ( !b || !b->fpSkin || !b->anims[CF_ANIM_FP_IDLE].count ) {
		return qfalse;
	}

	memset( &arms, 0, sizeof( arms ) );
	arms.reType = RT_MODEL;
	arms.hModel = b->model;
	arms.customSkin = b->fpSkin;
	arms.renderfx = gun->renderfx;
	arms.shaderRGBA[0] = (byte)( color[0] * 255 );
	arms.shaderRGBA[1] = (byte)( color[1] * 255 );
	arms.shaderRGBA[2] = (byte)( color[2] * 255 );
	arms.shaderRGBA[3] = 255;

	// Placed by reload progress, like the magazine; otherwise the one idle
	// frame. Nothing here steps with time, so a phase of our own is not kept.
	if ( reload >= 0.0f && b->anims[CF_ANIM_FP_RELOAD].count ) {
		anim = &b->anims[CF_ANIM_FP_RELOAD];
		phase = reload * (float)( anim->count - 1 );
	} else {
		anim = &b->anims[CF_ANIM_FP_IDLE];
		phase = 0.0f;
	}
	CG_RunAnim( anim, &phase, 0, &arms );

	if ( !CG_JointOnModel( &arms, "Weapon", &socket ) ) {
		return qfalse;
	}

	/*
	The grip, in the world, with the gun's axes made unit length again: the
	gun's are scaled by cg_gunScale, since that is how a model is scaled.
	*/
	gunScale = VectorLength( gun->axis[0] );
	if ( gunScale <= 0.0f ) {
		return qfalse;
	}
	VectorCopy( gun->origin, grip.origin );
	VectorMA( grip.origin, CF_GUN_GRIP_X, gun->axis[0], grip.origin );
	VectorMA( grip.origin, CF_GUN_GRIP_Y, gun->axis[1], grip.origin );
	VectorMA( grip.origin, CF_GUN_GRIP_Z, gun->axis[2], grip.origin );
	for ( i = 0; i < 3; i++ ) {
		VectorScale( gun->axis[i], 1.0f / gunScale, grip.axis[i] );
	}

	/*
	arms = grip * inverse(socket), with the socket's offset scaled up to match:
	the arms model is in body units, and it is about to be drawn k times
	bigger, so everything measured in it is k times further from the grip.
	*/
	k = gunScale / CF_BODY_UNITS_PER_METRE;
	CG_OrientInvert( &socket, &socket );
	VectorScale( socket.origin, k, socket.origin );
	CG_OrientCompose( &grip, &socket, &place );

	VectorCopy( place.origin, arms.origin );
	VectorCopy( place.origin, arms.oldorigin );
	for ( i = 0; i < 3; i++ ) {
		VectorScale( place.axis[i], k, arms.axis[i] );
	}
	arms.nonNormalizedAxes = qtrue;

	trap_R_AddRefEntityToScene( &arms );
	return qtrue;
}

/*
===============
CG_BodySocket

Where a named socket bone -- "Weapon", later hats and the rest -- is on a body
as it is drawn this frame, in world space: position AND axes, since a thing in
a hand has to turn with the hand. qfalse if this body has no such bone.

Asked with the same frames and blend the body is drawn with, so the socket is
where the hand IS on screen rather than where it was on the last whole frame.
===============
*/
qboolean CG_BodySocket( const refEntity_t *ent, const char *name, orientation_t *out ) {
	orientation_t tag, place;

	if ( !CG_JointOnModel( ent, name, &tag ) ) {
		return qfalse;
	}
	// The body's own transform, then the joint within it. Body entities are
	// never scaled, so the result's axes are unit length.
	VectorCopy( ent->origin, place.origin );
	VectorCopy( ent->axis[0], place.axis[0] );   // not AxisCopy: it takes no const
	VectorCopy( ent->axis[1], place.axis[1] );
	VectorCopy( ent->axis[2], place.axis[2] );
	CG_OrientCompose( &place, &tag, out );
	return qtrue;
}
