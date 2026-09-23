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

	/*
	A SPECTATOR KEEPS THE CROSSHAIR, and that used to be wrong.

	The reasoning here was "nothing to aim with when you are dead", which was
	true when being dead meant only watching. An eliminated player can now send
	his companion to a point by aiming at it, so the crosshair is the aim, and
	without it "go there" is a guess about where the middle of the screen is.

	Still hidden while genuinely holding no input -- an intermission or the
	freeze between rounds -- where it would be clutter over something you are
	only watching. PM_DEAD is the second and a half before the camera comes
	free, and there is nothing to point at yet.
	*/
	switch ( cg.predictedPlayerState.pm_type ) {
	case PM_INTERMISSION:
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
	/*
	Above the companion's bar rather than in the bottom-right corner, which the
	companion's bar now owns. The right-hand column reads downward: the gun, then
	the person holding the other side of the fight.

	Everything in the column is right-aligned to the same edge as the bar under
	it, so the block stays square as the numbers change width -- the old layout
	was laid out leftwards from a fixed point and a three-digit reserve ran off
	the screen.
	*/
	const float right = 640 - 30;
	const float y = 356;
	float       colour[4];
	int         inGun, reserve;
	char        inGunText[16];
	char        reserveText[16];
	float       reserveX, inGunX;

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

	// Real buffers rather than two va() calls: va returns one rotating static
	// string, and both of these have to be measured before either is drawn.
	Com_sprintf( inGunText, sizeof( inGunText ), "%i", inGun );
	Com_sprintf( reserveText, sizeof( reserveText ), "/ %i", reserve );

	reserveX = right - strlen( reserveText ) * 8;
	inGunX   = reserveX - 8 - strlen( inGunText ) * 14;

	CG_DrawString( inGunX, y, inGunText, 14, 26, colour );
	CG_DrawString( reserveX, y + 10, reserveText, 8, 16, hudDim );

	// The gun's name, so what you are holding is never a guess. Read from the
	// table, never written out here -- see cf_weapons.h.
	CG_DrawString( right - strlen( w->displayName ) * 6, y - 18,
	               w->displayName, 6, 12, hudDim );

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
CG_DrawHealthBar

One health bar: the bar, the number over it, and a label over that.

It is a bar and not just a number for the same reason the speed readout is: you
have to be able to read it without looking at it. The colour carries the same
information again, so that "am I in trouble" is answerable from peripheral
vision alone -- which is the entire requirement for the second bar, since
nobody is going to read their companion's health while being shot at.

`rightAlign` mirrors the text to the far edge of the bar so the companion's
block sits against the right side of the screen the way yours sits against the
left. Both are laid out from the same corner-relative numbers.
=================
*/
#define CF_HEALTH_BAR_W 170.0f
#define CF_HEALTH_BAR_H 10.0f

static void CG_DrawHealthBar( float x, float y, int health, int maxHealth,
                              const char *label, qboolean rightAlign ) {
	const float  w = CF_HEALTH_BAR_W;
	float        frac;
	const float *color;
	const char  *number;
	float        numberX, labelX;

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

	CG_FillRect( x, y, w, CF_HEALTH_BAR_H, hudDim );
	CG_FillRect( x, y, w * frac, CF_HEALTH_BAR_H, color );

	number = va( "%i", health );

	if ( rightAlign ) {
		numberX = x + w - strlen( number ) * 10;
		labelX  = x + w - strlen( label ) * 6;
	} else {
		numberX = x;
		labelX  = x;
	}

	CG_DrawString( numberX, y - 22, number, 10, 18, color );
	CG_DrawString( labelX,  y - 36, label,  6,  12, hudDim );
}

/*
=================
CG_DrawAllyOrders

The three slots she is currently under, as one line: "follow / free / auto".

THIS IS THE PART THAT MAKES THE COMMAND SYSTEM DEBUGGABLE BY THE PERSON USING
IT. Without it, "I told her to hold fire and she shot anyway" has three
indistinguishable causes -- she misheard, the order never arrived, or she is
doing exactly what was asked and the player misremembers asking. All three feel
like the game is broken. Showing her actual state separates them at a glance,
and that only matters more once a language model is the thing interpreting
speech and can quietly land on the wrong slot.

STUCK is drawn in the warning colour and replaces nothing: an order she cannot
carry out is still the order she is under, and the player needs both facts.
=================
*/
static void CG_DrawAllyOrders( float x, float y, int packed ) {
	// "going" rather than "goto": she keeps the order after arriving, so that a
	// spot she was sent to is one she returns to if something shoves her off it.
	static const char *moveNames[]   = { "follow", "hold", "push", "back", "going" };
	static const char *engageNames[] = { "free", "return", "hold fire" };
	static const char *focusNames[]  = { "auto", "target", "player", "bot" };
	const char *text;

	// 0 is "no companion, or a human teammate" -- G_UpdateAllyStatus only packs
	// this for a bot, because a person has orders of his own and inventing some
	// to display would be a readout that is simply false.
	if ( packed == 0 ) {
		return;
	}

	text = va( "%s / %s / %s",
	           moveNames[CF_ORDERS_MOVE( packed ) % ARRAY_LEN( moveNames )],
	           engageNames[CF_ORDERS_ENGAGE( packed ) % ARRAY_LEN( engageNames )],
	           focusNames[CF_ORDERS_FOCUS( packed ) % ARRAY_LEN( focusNames )] );

	CG_DrawString( x + CF_HEALTH_BAR_W - strlen( text ) * 6, y, text, 6, 12, hudDim );

	if ( CF_ORDERS_STUCK( packed ) ) {
		CG_DrawString( x + CF_HEALTH_BAR_W - 8 * 6, y + 13, "STUCK", 6, 12, hudOver );
	}
}

/*
=================
CG_DrawHealth

Both health bars: yours in the bottom-left corner, your companion's in the
bottom-right, with the orders she is under beneath hers.

Two bars rather than one because the companion is meant to be a fighter you make
decisions about, and every one of those decisions -- push, fall back, go and
help her -- turns on how much of her is left. A teammate whose condition you
cannot see is a teammate you can only react to after she is already dead.

Her bar is drawn from the ally fields in your OWN playerState, not from her
entity, so it stays correct while she is out of sight; see STAT_ALLY_CLIENT.
=================
*/
static void CG_DrawHealth( void ) {
	const playerState_t *ps = &cg.predictedPlayerState;
	const float y = 430;
	int         ally;

	if ( !cg_drawStatus.integer ) {
		return;
	}

	if ( ps->pm_type == PM_INTERMISSION ) {
		return;
	}

	/*
	YOUR bar goes away when you are dead. HERS DOES NOT.

	A spectator has no health worth showing, which is why this used to return
	here -- but that also took away the one thing an eliminated player still has
	a stake in. He can still give her orders, so he still needs to see whether
	she is winning the round he is no longer in, and what she currently thinks
	she was told. Hiding it is worst exactly when it matters most.

	A genuine spectator, who is on no side, has no ally and falls out below on
	STAT_ALLY_CLIENT being zero. Nothing special is needed for them.
	*/
	if ( ps->pm_type != PM_SPECTATOR ) {
		CG_DrawHealthBar( 30, y, ps->stats[STAT_HEALTH], ps->stats[STAT_MAX_HEALTH],
		                  "YOU", qfalse );
	}

	ally = ps->stats[STAT_ALLY_CLIENT] - 1;   // 0 means nobody; see cf_shared.h
	if ( ally < 0 || ally >= MAX_CLIENTS ) {
		return;
	}

	CG_DrawHealthBar( 640 - 30 - CF_HEALTH_BAR_W, y,
	                  ps->stats[STAT_ALLY_HEALTH], ps->stats[STAT_ALLY_MAX_HEALTH],
	                  cgs.clientinfo[ally].infoValid ? cgs.clientinfo[ally].name
	                                                 : "COMPANION",
	                  qtrue );

	CG_DrawAllyOrders( 640 - 30 - CF_HEALTH_BAR_W, y + CF_HEALTH_BAR_H + 4,
	                   ps->stats[STAT_ALLY_ORDERS] );
}

/*
=================
CG_DrawAllyAck

What she said back, for a moment after she said it.

Deliberately transient. It is a confirmation, not status -- the standing state
is the line above, which is always there. Something that stayed on screen would
stop being read within a round.

Drawn even when STAT_ALLY_ORDERS says nothing, because a REFUSAL is exactly the
case where there is no order to show and the player most needs to hear why.
=================
*/
#define CF_ACK_TIME 2500

static void CG_DrawAllyAck( void ) {
	float alpha;
	vec4_t color;

	if ( !cg.allyAck[0] ) {
		return;
	}

	alpha = 1.0f - (float)( cg.time - cg.allyAckTime ) / CF_ACK_TIME;
	if ( alpha <= 0.0f ) {
		cg.allyAck[0] = '\0';
		return;
	}
	if ( alpha > 1.0f ) {
		alpha = 1.0f;   // a clock that jumped backwards; do not brighten past full
	}

	color[0] = hudAccent[0];
	color[1] = hudAccent[1];
	color[2] = hudAccent[2];
	color[3] = alpha;

	// Right-aligned to the same edge as her health bar, so her voice comes from
	// where the rest of her is on the HUD.
	CG_DrawString( 640 - 30 - strlen( cg.allyAck ) * 8, 392,
	               cg.allyAck, 8, 16, color );
}

/*
=================
CG_DrawTalkHint

"Hold B to talk to Vex", in the quiet parts of a match.

THE ONE THING IN THIS GAME NOTHING ELSE ANNOUNCES. Every other input is either
obvious (you press forward and you move) or discoverable from what is on the
HUD (her order slots say she takes orders). Talking to her is the mechanic the
whole project is built around and there is no way to find out it exists.

The key is read from cl_voiceKey rather than assumed, and that is not
fussiness: a hint that names the wrong key is worse than no hint, and this is a
game where bindings have already been silently lost once. If the action is
bound to nothing the hint says so instead, because an unbound push-to-talk is
otherwise completely invisible -- the migration deliberately refuses to take a
default key that is already in use, and this is the only place that would ever
surface it.

Shown only while she can actually hear him (cl_voiceState ready), and NEVER
during a live round. A firefight is not where you read UI text, and the moment
he is being shot at is exactly when unnecessary words on screen stop being
help. Warmup and the gaps between rounds are when a new player is looking
around anyway.

IT DOES NOT RETIRE ONCE HE HAS USED IT, and that was the first version's
mistake. The obvious design is to show a control hint until the player has
found the control, which is right for a hint about a jump key -- and wrong
here. Talking to her IS the game. A player who comes back after a fortnight and
cannot remember how to speak to his companion has lost the entire point of it,
and the cost of reminding him is one dim line on a screen where nothing is
happening.

So the only thing that hides it is a live round, where words on screen stop
being help. Everywhere else it stays.
=================
*/
static void CG_DrawTalkHint( void ) {
	char   state[32], key[32], text[128];
	vec4_t color;

	if ( cgs.matchState == MS_LIVE ) {
		return;
	}

	trap_Cvar_VariableStringBuffer( "cl_voiceState", state, sizeof( state ) );
	if ( Q_stricmp( state, "ready" ) ) {
		return;   // she is off, still loading, or failed -- nothing to offer
	}

	/*
	"your companion" rather than her name, and that is not laziness.

	The name on her client slot is "red companion" -- G_NameCompanionsForTheirSides
	generates it from the side she is on, and it stays that on a matchmade server
	too, because the display name signed into the match ticket is the PLAYER's.
	The name she actually answers to lives in cfvoice's persona, which is a
	proprietary prompt file on the other side of the helper boundary and is not
	something cgame can see or should learn.

	So the choice was "Hold b to talk to red companion", which reads like a
	debug string, or a phrase that is true regardless. If her name ever does
	cross the boundary this is one line.
	*/
	trap_Cvar_VariableStringBuffer( "cl_voiceKey", key, sizeof( key ) );

	if ( key[0] ) {
		Com_sprintf( text, sizeof( text ), "Hold %s to talk to your companion", key );
	} else {
		Com_sprintf( text, sizeof( text ),
		             "No key talks to your companion -- set one in CONTROLS" );
	}

	color[0] = hudAccent[0];
	color[1] = hudAccent[1];
	color[2] = hudAccent[2];
	color[3] = 0.85f;

	/*
	Above the health bars at 430 and clear of the acknowledgment at 392, which
	is right-aligned while this is centred. The first version sat at 430 and
	drew straight through both health bars.
	*/
	CG_DrawStringCentred( 320, 370, text, 7, 14, color );
}

/*
=================
CG_DrawSubtitle

What she just said, in text, because otherwise she only ever says it out loud.

THE REASON THIS EXISTS is not polish. Her dialogue was audio-only: the engine
printed it to the console and spoke it through cftts, and nothing put it on
screen. A deaf or hard-of-hearing player therefore got a companion who never
communicated -- and natural-language command of that companion is the thing
design/README.md names as the entire differentiator of the game. The feature
was not degraded for those players, it was absent.

It is also the only route by which her dialogue could ever be localised. You
cannot translate speech you do not hold as text, and the text was being thrown
away after one Com_Printf.

TIMED BY LENGTH rather than by the audio. cgame cannot see how long the WAV
runs -- and there may not be one, since speech synthesis is optional in exactly
the way speech recognition is. Reading speed is the honest fallback and it is
the right one anyway: a subtitle is for reading, and somebody who cannot hear
the audio has no use for a subtitle that matches its duration.
=================
*/
/*
52, NOT the ~76 that would fit. At 8px per character in a 640-wide space this
is 416px centred, so a full line runs from x=112 to x=528 and stops short of
the ammo column on the right, which begins around x=560.

Found by looking at it: at 62 characters the first line ended a few pixels from
"Glock 17" on the same baseline, and the two read as a single row of text
rather than as a subtitle and a HUD element. Nothing overlapped; it was simply
unreadable as two things.
*/
#define CF_SUB_LINE_CHARS 52
#define CF_SUB_MAX_LINES  4
#define CF_SUB_BASE_MS    1800    // on screen even for a two-word answer
#define CF_SUB_PER_CHAR   55      // roughly 18 characters a second
#define CF_SUB_MAX_MS     9000
#define CF_SUB_FADE_MS    500
#define CF_SUB_BOTTOM     352     // clear of the talk hint at 370

static void CG_DrawSubtitle( void ) {
	char   lines[CF_SUB_MAX_LINES][CF_SUB_LINE_CHARS + 1];
	int    numLines = 0;
	int    seq, age, life, len, i;
	float  alpha;
	vec4_t color;
	const char *p;

	if ( !cg_subtitles.integer ) {
		return;
	}

	/*
	Asked every frame. It is a bounded string copy behind a function call, and
	polling means the arrival of a line is noticed in the frame that draws it
	rather than whenever the engine happened to be mid-something.
	*/
	{
		char now[MAX_SUBTITLE_TEXT];

		seq = trap_VoiceSubtitle( now, sizeof( now ) );

		if ( seq != cg.subtitleSeq ) {
			cg.subtitleSeq = seq;
			cg.subtitleTime = cg.time;
			Q_strncpyz( cg.subtitle, now, sizeof( cg.subtitle ) );
		}
	}

	if ( !cg.subtitle[0] || !cg.subtitleTime ) {
		return;
	}

	len = (int)strlen( cg.subtitle );
	life = CF_SUB_BASE_MS + len * CF_SUB_PER_CHAR;
	if ( life > CF_SUB_MAX_MS ) {
		life = CF_SUB_MAX_MS;
	}

	age = cg.time - cg.subtitleTime;
	if ( age < 0 || age >= life ) {
		return;      // expired, or the clock went backwards
	}

	alpha = 1.0f;
	if ( age > life - CF_SUB_FADE_MS ) {
		alpha = (float)( life - age ) / CF_SUB_FADE_MS;
	}

	/*
	Wrapped on word boundaries. Breaking mid-word is the one thing that makes a
	subtitle read as broken rather than as terse, and her lines are prose.

	A word longer than a whole line is broken rather than dropped -- it cannot
	be wrapped anywhere, and losing it silently would be worse than an ugly
	break nobody will ever see.
	*/
	p = cg.subtitle;
	while ( *p && numLines < CF_SUB_MAX_LINES ) {
		int take = 0, lastSpace = -1;

		while ( p[take] && take < CF_SUB_LINE_CHARS ) {
			if ( p[take] == ' ' ) {
				lastSpace = take;
			}
			take++;
		}
		if ( p[take] && lastSpace > 0 ) {
			take = lastSpace;
		}

		memcpy( lines[numLines], p, take );
		lines[numLines][take] = '\0';
		numLines++;

		p += take;
		while ( *p == ' ' ) {
			p++;
		}
	}

	color[0] = 1.0f;
	color[1] = 1.0f;
	color[2] = 1.0f;
	color[3] = alpha;

	// Grows upward from a fixed bottom edge, so a two-line answer and a
	// four-line one both end at the same height and the eye does not have to
	// go looking for where the text starts.
	for ( i = 0; i < numLines; i++ ) {
		CG_DrawStringCentred( 320, CF_SUB_BOTTOM - ( numLines - 1 - i ) * 18,
		                      lines[i], 8, 16, color );
	}
}

/*
=================
CG_DrawDeathCam

Who it was, while the death camera turns to show them. Names the killer from
the scoreboard's own record of them, so it agrees with the kill feed; a death
with nobody to blame -- a fall, or our own doing -- just says so.
=================
*/
static void CG_DrawDeathCam( void ) {
	const char *line;

	if ( !cg.deathCam ) {
		return;
	}
	if ( cg.killer >= 0 && cg.killer < MAX_CLIENTS && cg.killer != cg.snap->ps.clientNum
	     && cgs.clientinfo[cg.killer].infoValid ) {
		line = va( "KILLED BY %s", cgs.clientinfo[cg.killer].name );
	} else {
		line = "YOU DIED";
	}
	// Low, above the health bars: the top of the screen has the round banner,
	// and the middle is where the camera is showing the killer.
	CG_DrawStringCentred( 320, 380, line, 10, 20, hudAccent );
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
		// No crosshair while the camera is out of our head: it would sit in the
		// middle of the screen pointing at nothing we are aiming.
		if ( !cg.deathCam ) {
			CG_DrawCrosshair();
		}
		CG_DrawDeathCam();
		CG_DrawHitMarker();
		CG_DrawSpeed();
		CG_DrawHealth();
		CG_DrawAllyAck();
		CG_DrawAmmo();
		CG_DrawRoundStatus();
		CG_DrawTalkHint();
		CG_DrawSubtitle();
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
