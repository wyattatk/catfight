/*
===========================================================================
catfight -- 2D overlay.

catfight has no font yet, so everything here is built out of rectangles. That
is a real constraint rather than a placeholder excuse: a speed readout drawn as
a bar is arguably better for tuning movement than a number, because you watch
it out of the corner of your eye while playing.
===========================================================================
*/

#include "cg_local.h"

static const float hudWhite[4]  = { 1.0f, 1.0f, 1.0f, 1.0f };
static const float hudDim[4]    = { 1.0f, 1.0f, 1.0f, 0.25f };
static const float hudAccent[4] = { 1.0f, 0.75f, 0.35f, 0.85f };
static const float hudOver[4]   = { 1.0f, 0.40f, 0.30f, 0.85f };

/*
================
CG_AdjustFrom640

Everything in this file is laid out in a virtual 640x480 space and scaled to
the real window here, so the HUD is the same size regardless of resolution.
================
*/
void CG_AdjustFrom640( float *x, float *y, float *w, float *h ) {
	*x *= cgs.screenXScale;
	*y *= cgs.screenYScale;
	*w *= cgs.screenXScale;
	*h *= cgs.screenYScale;
}

void CG_FillRect( float x, float y, float width, float height, const float *color ) {
	trap_R_SetColor( color );

	CG_AdjustFrom640( &x, &y, &width, &height );
	trap_R_DrawStretchPic( x, y, width, height, 0, 0, 0, 0, cgs.media.white );

	trap_R_SetColor( NULL );
}

/*
================
CG_DrawString

The font sheet is a 16x16 grid of cells indexed by byte value, matching the
engine's own console drawing (cl_scrn.c).
================
*/
void CG_DrawString( float x, float y, const char *s, float charWidth, float charHeight,
                    const float *color ) {
	float ax, ay, aw, ah;
	float frow, fcol;
	int   ch;

	trap_R_SetColor( color );

	while ( *s ) {
		ch = *s & 255;

		if ( ch != ' ' ) {
			fcol = ( ch & 15 ) * 0.0625f;
			frow = ( ch >> 4 ) * 0.0625f;

			ax = x;
			ay = y;
			aw = charWidth;
			ah = charHeight;
			CG_AdjustFrom640( &ax, &ay, &aw, &ah );

			trap_R_DrawStretchPic( ax, ay, aw, ah,
			                       fcol, frow, fcol + 0.0625f, frow + 0.0625f, cgs.media.charset );
		}

		x += charWidth;
		s++;
	}

	trap_R_SetColor( NULL );
}

void CG_DrawStringCentred( float cx, float y, const char *s, float charWidth, float charHeight,
                           const float *color ) {
	CG_DrawString( cx - strlen( s ) * charWidth * 0.5f, y, s, charWidth, charHeight, color );
}

/*
=================
CG_DrawCrosshair
=================
*/
static void CG_DrawCrosshair( void ) {
	const float thickness = 2;
	const float length = 10;
	const float gap = 4;
	float       cx, cy;

	if ( !cg_drawCrosshair.integer ) {
		return;
	}

	// nothing to aim with when you are dead, spectating, or held still between
	// rounds -- a crosshair there is just clutter over the thing you are watching
	switch ( cg.predictedPlayerState.pm_type ) {
	case PM_INTERMISSION:
	case PM_SPECTATOR:
	case PM_DEAD:
	case PM_FREEZE:
		return;
	default:
		break;
	}

	cx = 320;
	cy = 240;

	CG_FillRect( cx - gap - length, cy - thickness / 2, length, thickness, hudWhite );
	CG_FillRect( cx + gap,          cy - thickness / 2, length, thickness, hudWhite );
	CG_FillRect( cx - thickness / 2, cy - gap - length, thickness, length, hudWhite );
	CG_FillRect( cx - thickness / 2, cy + gap,          thickness, length, hudWhite );
}

/*
=================
CG_DrawHitMarker

Four short diagonal-ish ticks around the crosshair for a moment after one of our
bullets lands on somebody.

A hitscan weapon gives the shooter nothing to watch: no travel, no arc, and at
range the victim's reaction is a few pixels. Without this the difference between
a hit and a miss is invisible, and a gun you cannot tell you are hitting with
feels broken however good its numbers are.
=================
*/
#define HITMARKER_MSEC 220

static void CG_DrawHitMarker( void ) {
	const float thickness = 2;
	const float length    = 5;
	const float gap       = 9;
	float       age, fade;
	float       colour[4];
	float       cx = 320, cy = 240;

	if ( !cg.hitMarkerTime ) {
		return;
	}

	age = (float)( cg.time - cg.hitMarkerTime );
	if ( age < 0 || age > HITMARKER_MSEC ) {
		return;
	}

	// Fades out rather than blinking off, so it reads as a hit landing rather
	// than as the HUD flickering.
	fade = 1.0f - ( age / (float)HITMARKER_MSEC );

	colour[0] = 1.0f;
	colour[1] = 1.0f;
	colour[2] = 1.0f;
	colour[3] = fade;

	CG_FillRect( cx - gap - length, cy - gap, length, thickness, colour );
	CG_FillRect( cx + gap,          cy - gap, length, thickness, colour );
	CG_FillRect( cx - gap - length, cy + gap, length, thickness, colour );
	CG_FillRect( cx + gap,          cy + gap, length, thickness, colour );
}

/*
=================
CG_DrawAmmo

Rounds in the gun, and rounds left over.

Shown as two numbers of different weight because they are two different
questions -- "can I finish this fight" and "can I keep fighting" -- and a
17+1 magazine is not something a player can feel without being able to see it
count down. Turns red when the magazine is nearly out, since running dry
mid-fight costs the slow reload.
=================
*/
static void CG_DrawAmmo( void ) {
	const playerState_t *ps = &cg.predictedPlayerState;
	const cf_weaponInfo_t *w;
	const float x = 560;
	const float y = 430;
	float       colour[4];
	int         inGun, reserve;

	if ( ps->weapon == WP_NONE ) {
		return;
	}
	switch ( ps->pm_type ) {
	case PM_INTERMISSION:
	case PM_SPECTATOR:
	case PM_DEAD:
		return;
	default:
		break;
	}

	w       = CF_Weapon( ps->weapon );
	inGun   = ps->ammo[ps->weapon];
	reserve = ps->stats[STAT_RESERVE];

	Vector4Copy( hudWhite, colour );
	if ( inGun == 0 ) {
		Vector4Copy( hudOver, colour );
	} else if ( inGun <= w->magazine / 4 ) {
		Vector4Copy( hudAccent, colour );
	}

	CG_DrawString( x, y, va( "%i", inGun ), 14, 26, colour );
	CG_DrawString( x + 46, y + 10, va( "/ %i", reserve ), 8, 16, hudDim );

	// The gun's name, so what you are holding is never a guess. Read from the
	// table, never written out here -- see cf_weapons.h.
	CG_DrawString( x - 4, y - 18, w->displayName, 6, 12, hudDim );

	if ( ps->weaponstate == WEAPON_RELOADING ) {
		CG_DrawStringCentred( 320, 300, "RELOADING", 8, 16, hudDim );
	}
}

/*
=================
CG_DrawSpeed

Horizontal speed as a bar, with a tick at the run speed. Watching where the bar
sits relative to the tick is how you tell whether the movement tuning is doing
what you meant it to.
=================
*/
static void CG_DrawSpeed( void ) {
	const float x = 220;
	const float y = 430;
	const float w = 200;
	const float h = 6;
	float       speed;
	float       runSpeed;
	float       frac;
	vec3_t      horizontal;

	if ( !cg_showSpeed.integer ) {
		return;
	}

	VectorCopy( cg.predictedPlayerState.velocity, horizontal );
	horizontal[2] = 0;
	speed = VectorLength( horizontal );

	runSpeed = cg.predictedPlayerState.speed;
	if ( runSpeed <= 0 ) {
		runSpeed = CF_RUN_SPEED;
	}

	// the bar runs to twice the run speed, so anything above the tick is
	// momentum the movement code let you keep
	frac = speed / ( runSpeed * 2.0f );
	if ( frac > 1.0f ) {
		frac = 1.0f;
	}

	CG_FillRect( x, y, w, h, hudDim );
	CG_FillRect( x, y, w * frac, h, speed > runSpeed + 1 ? hudOver : hudAccent );

	// the run speed tick
	CG_FillRect( x + w * 0.5f - 1, y - 3, 2, h + 6, hudWhite );

	CG_DrawStringCentred( x + w * 0.5f, y - 22, va( "%i", (int)speed ), 8, 16,
	                      speed > runSpeed + 1 ? hudOver : hudAccent );
}

/*
=================
CG_DrawHealth

Health as a bar in the bottom left, with the number over it.

It is a bar and not just a number for the same reason the speed readout is: you
have to be able to read it without looking at it. The colour carries the same
information again, so that "am I in trouble" is answerable from peripheral
vision alone.
=================
*/
static void CG_DrawHealth( void ) {
	const float x = 30;
	const float y = 430;
	const float w = 140;
	const float h = 10;
	int         health;
	int         maxHealth;
	float       frac;
	const float *color;

	if ( !cg_drawStatus.integer ) {
		return;
	}

	// a spectator has no health worth showing
	if ( cg.predictedPlayerState.pm_type == PM_SPECTATOR
	     || cg.predictedPlayerState.pm_type == PM_INTERMISSION ) {
		return;
	}

	health = cg.predictedPlayerState.stats[STAT_HEALTH];
	maxHealth = cg.predictedPlayerState.stats[STAT_MAX_HEALTH];
	if ( maxHealth <= 0 ) {
		maxHealth = CF_MAX_HEALTH;
	}

	if ( health < 0 ) {
		health = 0;
	}

	frac = (float)health / (float)maxHealth;
	if ( frac > 1.0f ) {
		frac = 1.0f;
	}

	if ( frac <= 0.25f ) {
		color = hudOver;
	} else if ( frac <= 0.5f ) {
		color = hudAccent;
	} else {
		color = hudWhite;
	}

	CG_FillRect( x, y, w, h, hudDim );
	CG_FillRect( x, y, w * frac, h, color );

	CG_DrawString( x, y - 22, va( "%i", health ), 10, 18, color );
}

/*
=================
CG_Draw2D
=================
*/
void CG_Draw2D( void ) {
	if ( trap_Key_GetCatcher() & KEYCATCH_UI ) {
		return;
	}

	// the scoreboard replaces the HUD rather than sitting on top of it
	if ( !CG_DrawScoreboard() ) {
		CG_DrawCrosshair();
		CG_DrawHitMarker();
		CG_DrawSpeed();
		CG_DrawHealth();
		CG_DrawAmmo();
		CG_DrawRoundStatus();
	}

	// Last, over whichever of those drew. Outside the branch on purpose: the
	// postgame holds the keyboard, so its prompt has to be on screen even for
	// somebody who has turned the scoreboard off.
	CG_DrawPostgamePrompt();
}

/*
=================
CG_DrawInformation

Shown while we are connected but have not received a snapshot yet. Without a
font there is nothing to say, so this just fills the screen so the player can
see the game is alive rather than staring at a black window.
=================
*/
void CG_DrawInformation( void ) {
	const float bg[4] = { 0.06f, 0.07f, 0.09f, 1.0f };
	const char *name;
	float       phase;

	CG_FillRect( 0, 0, 640, 480, bg );

	CG_DrawStringCentred( 320, 200, "LOADING", 12, 24, hudAccent );

	// prefer the mapper's own title for the level, and fall back to the file
	// name, which is at least always there
	name = CG_ConfigString( CS_LEVEL_NAME );
	if ( !name[0] ) {
		name = cgs.mapname;
	}
	CG_DrawStringCentred( 320, 236, name, 8, 16, hudDim );

	// a slow sweep, so a hang is visibly different from slow loading
	phase = ( trap_Milliseconds() % 1200 ) / 1200.0f;
	CG_FillRect( 220 + 200 * phase, 300, 20, 4, hudAccent );
}
