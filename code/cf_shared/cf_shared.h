/*
===========================================================================
catfight -- definitions shared by the game, cgame and ui modules.

This is catfight's replacement for Quake 3's bg_public.h. Everything in here
is *gameplay* vocabulary that the three game modules have to agree on.

It deliberately does not describe anything the engine cares about: the engine
owns q_shared.h (playerState_t, entityState_t, usercmd_t and the netfield
tables in msg.c), and those structures are fixed by the network protocol. What
we put *into* those fields is ours.
===========================================================================
*/

#ifndef CF_SHARED_H
#define CF_SHARED_H

#include "../qcommon/q_shared.h"

#define CF_GAME_VERSION "catfight 0.1"

#define MAX_NETNAME 36

/*
==============================================================================

CONFIGSTRINGS

The engine reserves 0 and 1. Everything from RESERVED_CONFIGSTRINGS up is ours
to allocate, and is reliably replicated to every client.

==============================================================================
*/

#define CS_LEVEL_NAME       (RESERVED_CONFIGSTRINGS + 0)  // map title for the loading screen
#define CS_LEVEL_START_TIME (RESERVED_CONFIGSTRINGS + 1)  // level.startTime, so clients can show a match clock

/*
The match state, as "<state> <stateTime> <endTime>".

This is a configstring rather than a server command because a client that joins
mid-match, or reconnects, must arrive already knowing what is going on. Server
commands only reach whoever was listening when they were sent; configstrings are
part of the gamestate every client is handed on connect.
*/
#define CS_MATCH_STATE      (RESERVED_CONFIGSTRINGS + 2)

// "<redRoundsWon> <blueRoundsWon> <roundNumber>"
#define CS_SCORES           (RESERVED_CONFIGSTRINGS + 3)

#define CS_MODELS           (RESERVED_CONFIGSTRINGS + 8)
#define CS_SOUNDS           (CS_MODELS + MAX_MODELS)
#define CS_PLAYERS          (CS_SOUNDS + MAX_SOUNDS)

#define CS_MAX              (CS_PLAYERS + MAX_CLIENTS)

#if CS_MAX > MAX_CONFIGSTRINGS
#error overflowed MAX_CONFIGSTRINGS
#endif

/*
==============================================================================

ENTITY TYPES

entityState_t.eType. The client decides how to draw an entity from this.

==============================================================================
*/

typedef enum {
	ET_GENERAL,
	ET_PLAYER,
	ET_ITEM,
	ET_MOVER,

	ET_EVENTS   // any eType >= ET_EVENTS is an event, see cf_event_t
} entityType_t;

/*
==============================================================================

TEAMS

catfight is played as two sides. Right now a side holds exactly one player, so
in practice it is a duel -- but the rules are written in terms of *teams* from
the start, because "the round ends when a side has nobody left alive" is the
same sentence whether a side is one player or four. Adding bot teammates later
is then a matter of putting more clients on a team, not of rewriting what a
round is.

TEAM_FREE exists for warmup, where there are no sides and everyone is just
running around the map.

==============================================================================
*/

typedef enum {
	TEAM_FREE,        // no side -- warmup, or not yet assigned
	TEAM_RED,
	TEAM_BLUE,
	TEAM_SPECTATOR,

	TEAM_NUM_TEAMS
} team_t;

/*
==============================================================================

MATCH STRUCTURE

A match is a sequence of rounds. A round is: everyone alive, fight, and the
side with anyone left standing wins it. Dying does not put you back in the
round -- it puts you out of it until the next one starts.

WARMUP is the exception and it is deliberate: with nobody to fight, or while
waiting for a second player, the map is a practice space and death just respawns
you. That is what makes the game testable by one person.

==============================================================================
*/

/*
These values go over the wire in CS_MATCH_STATE, so append rather than insert.
*/
typedef enum {
	MS_WARMUP,      // not enough players for a match; free respawn
	MS_COUNTDOWN,   // both sides present, round about to begin
	MS_LIVE,        // round in progress
	MS_ROUND_END,   // a side has won the round; short pause before the next
	MS_MATCH_END,   // a side has won the match
	MS_PAUSED       // a side emptied mid-match; holding it for them to come back
} matchState_t;

/*
==============================================================================

PLAYER STATE FIELDS

playerState_t.stats / persistant. Indices are ours.

stats are per-life and are reset on every spawn. persistant survives death and
is what the scoreboard is built from.

==============================================================================
*/

typedef enum {
	STAT_HEALTH,
	STAT_ARMOR,
	STAT_MAX_HEALTH,

	// Rounds not in the gun. What IS in the gun lives in ps->ammo[weapon],
	// which is where the engine already expects to find it.
	STAT_RESERVE,

	/*
	Accumulated muzzle rise, in hundredths of a degree.

	It is state rather than a one-off kick because that is what makes rapid
	fire walk up the target and a measured pace not: each shot adds, and it
	bleeds off continuously. Kept in playerState so pmove can predict it on the
	client and arrive at the same number the server does.
	*/
	STAT_RECOIL
} statIndex_t;

typedef enum {
	PERS_SCORE,       // rounds this player was alive at the end of
	PERS_KILLS,
	PERS_DEATHS,
	PERS_TEAM,        // team_t -- so the local client can colour its own HUD
	PERS_ELIMINATED   // out of the current round
} persEnum_t;

/*
==============================================================================

MOVEMENT

pmove is the single piece of code that both the server and the client run over
the same inputs: the server to decide where you really are, the client to
predict it locally so the game does not feel like it is happening by post. Both
sides must produce bit-identical results from identical input, which is why
everything in here is integer or snapped float.

==============================================================================
*/

// player bounding box, in units. The mapping guide quotes these numbers, so
// changing them means changing mapping/README.md too.
#define CF_PLAYER_WIDTH      32
#define CF_PLAYER_HEIGHT     56
#define CF_PLAYER_CROUCH_HEIGHT 40

#define CF_MINS_X            (-CF_PLAYER_WIDTH / 2)
#define CF_MINS_Y            (-CF_PLAYER_WIDTH / 2)
#define CF_MINS_Z            -24    // origin sits 24 units above the feet
#define CF_MAXS_X            (CF_PLAYER_WIDTH / 2)
#define CF_MAXS_Y            (CF_PLAYER_WIDTH / 2)
#define CF_MAXS_Z            (CF_MINS_Z + CF_PLAYER_HEIGHT)
#define CF_CROUCH_MAXS_Z     (CF_MINS_Z + CF_PLAYER_CROUCH_HEIGHT)

#define CF_VIEWHEIGHT_STAND  26
#define CF_VIEWHEIGHT_CROUCH 12
#define CF_VIEWHEIGHT_DEAD   -16

/*
Movement tuning. These are the numbers that decide how catfight feels, so they
live together in one block where they can be read as a set rather than hunted
for across a thousand lines of movement code.

Speeds are units/second, accelerations are units/second per (unit/second of
error) -- i.e. the fraction of the remaining speed error closed per second.
*/
#define CF_GRAVITY            800.0f   // units/sec^2
#define CF_JUMP_VELOCITY      265.0f   // ~44 units of jump height under CF_GRAVITY

#define CF_RUN_SPEED          300.0f
#define CF_CROUCH_SPEED       130.0f
#define CF_WALK_SPEED_SCALE   0.45f    // held +speed / BUTTON_WALKING

#define CF_GROUND_ACCEL       14.0f
#define CF_GROUND_FRICTION    8.0f
#define CF_STOP_SPEED         100.0f   // below this, friction is applied at a flat rate
                                       // so you actually come to rest instead of creeping

/*
Air movement is the knob that decides whether catfight has strafe-jumping.

Accelerating toward where you are pointing, with no ceiling on the result, is
the whole mechanism behind Quake's strafe-jump: push sideways while turning and
the acceleration is always perpendicular to your velocity, so it adds speed
instead of redirecting it, forever.

catfight accelerates the same way -- that part is just how you steer a body in
flight -- but clamps the result: air control may redirect your momentum and may
bring you up to run speed, and may not build speed beyond what you already had.
Momentum from somewhere else (a jump pad, an explosion) survives the clamp;
momentum you try to manufacture by strafing does not.

Deleting that clamp is what would turn catfight into a strafe-jumping game.
That is a real design decision and it is deliberately not made by default.
*/
#define CF_AIR_ACCEL          2.5f
#define CF_AIR_CONTROL_CLAMP  1

#define CF_STEP_HEIGHT        18.0f    // stairs you climb without jumping
#define CF_MIN_WALK_NORMAL    0.7f     // steeper than this and you slide down it
#define CF_OVERCLIP           1.001f   // push out of planes slightly, to avoid re-hitting them

// pm_type
typedef enum {
	PM_NORMAL,        // can accelerate and turn
	PM_NOCLIP,        // noclip movement
	PM_SPECTATOR,     // flying, no clipping against the world
	PM_DEAD,          // no acceleration or turning, but still falls
	PM_FREEZE,        // stuck in place, no movement at all
	PM_INTERMISSION   // no movement or looking around
} pmtype_t;

// playerState_t.pm_flags
#define PMF_DUCKED       1
#define PMF_JUMP_HELD    2
#define PMF_RESPAWNED    4    // cleared when the client acknowledges the respawn

/*
The trigger is still held from the last shot.

This is what makes a semi-automatic semi-automatic: the trigger has to RESET
before it can fire again, so holding the button down gives exactly one shot.
Without it a "semi" weapon is just an automatic with a slow rate of fire, which
is the single most common way a game gets a pistol wrong.
*/
#define PMF_ATTACK_HELD  8

// Same idea for the reload key: holding it must not queue reload after reload.
#define PMF_RELOAD_HELD  16

#define MAXTOUCH 32

typedef struct {
	// state, modified in place
	playerState_t *ps;
	usercmd_t      cmd;
	int            tracemask;      // collide against these contents

	// results
	int            numtouch;
	int            touchents[MAXTOUCH];

	vec3_t         mins, maxs;     // bounding box in use this move

	qboolean       noFootsteps;

	// callbacks, so pmove does not have to know whether it is running on the
	// server (trap_Trace) or on the client (trap_CM_BoxTrace)
	void (*trace)( trace_t *results, const vec3_t start, const vec3_t mins,
	               const vec3_t maxs, const vec3_t end, int passEntityNum, int contentMask );
	int  (*pointcontents)( const vec3_t point, int passEntityNum );
} pmove_t;

void CF_Pmove( pmove_t *pmove );
void CF_PlayerBounds( const playerState_t *ps, vec3_t mins, vec3_t maxs );
void CF_UpdateViewAngles( playerState_t *ps, const usercmd_t *cmd );
void CF_PlayerStateToEntityState( playerState_t *ps, entityState_t *s, qboolean snap );

// contents a player collides with
#define MASK_PLAYERSOLID (CONTENTS_SOLID | CONTENTS_PLAYERCLIP | CONTENTS_BODY)
#define MASK_SOLID       (CONTENTS_SOLID)
#define MASK_DEADSOLID   (CONTENTS_SOLID | CONTENTS_PLAYERCLIP)

/*
What a bullet stops on.

Deliberately NOT MASK_PLAYERSOLID: playerclip is invisible geometry that exists
to keep bodies out of places, and a bullet stopping in mid-air on it is the
classic "my shot hit nothing" bug that takes a day to find because there is
nothing on screen where the bullet died.
*/
#define MASK_SHOT        (CONTENTS_SOLID | CONTENTS_BODY)

/*
==============================================================================

ENTITY FLAGS

entityState_t.eFlags. Facts about an entity that the client needs in order to
draw it correctly, and which are cheaper to send as bits than as their own
fields.

==============================================================================
*/

#define EF_DEAD        0x00000001   // this player is a corpse
#define EF_TELEPORT    0x00000002   // moved discontinuously; do not interpolate

/*
==============================================================================

EVENTS

The one channel the server has for telling clients that something *happened*,
as opposed to what merely *is*. State replication says "this player is at X with
Y health"; it cannot say "this player was hit", because by the time the snapshot
arrives the hit is over and only its consequences are in the state.

Two ways an event reaches a client, both ending up in entityState_t.event:
  - PM_AddEvent, from inside movement code, rides out in the playerState and is
    copied across by CF_PlayerStateToEntityState. Use it for anything a player's
    own movement causes, because prediction then fires it locally with no delay.
  - G_AddEvent / G_TempEntity, from the game module, attaches an event to a real
    entity or to a throwaway one at a position. Use it for anything the server
    decides.

==============================================================================
*/

/*
An event number is sent in the low 8 bits of entityState_t.event, with a 2-bit
sequence above it. Two identical events in a row would otherwise look to the
client like a field that never changed, and the second would be dropped; a
rolling sequence makes a repeat read as new. cf_entstate.c uses the same scheme
for playerState events, and the two must agree.
*/
#define EV_EVENT_BIT1     0x00000100
#define EV_EVENT_BIT2     0x00000200
#define EV_EVENT_BITS     (EV_EVENT_BIT1 | EV_EVENT_BIT2)

// an event older than this is stale and is not replayed to a client that has
// only just started receiving snapshots
#define EVENT_VALID_MSEC  300

typedef enum {
	EV_NONE,

	// movement, raised by pmove on both sides
	EV_FOOTSTEP,
	EV_FALL_SHORT,
	EV_FALL_MEDIUM,
	EV_FALL_FAR,
	EV_JUMP,

	// combat, raised by the server
	EV_PAIN,
	EV_DEATH,

	// spawning and teleporting
	EV_PLAYER_TELEPORT_IN,
	EV_PLAYER_TELEPORT_OUT,

	// match structure
	EV_ROUND_START,
	EV_ROUND_WON,
	EV_ROUND_LOST,

	/*
	Weapons. EV_FIRE is raised by PMOVE, not by the server, and that is the
	whole reason firing feels immediate: the client predicts it on the frame
	the button went down rather than waiting for the round trip. The server
	raises the same event from the same code and is the one that decides what
	the bullet hit.

	EV_DRY_FIRE is the click of an empty gun. It is a real event rather than
	silence because "I am out" needs to reach the player somehow, and on a
	pistol whose slide has locked back it is the most recognisable sound there
	is.
	*/
	EV_FIRE,
	EV_DRY_FIRE,
	EV_RELOAD,            // parm: qtrue if the slide was locked back
	EV_RELOAD_DONE,

	// Impacts, raised by the server once it knows what was hit.
	EV_BULLET_FLESH,
	EV_BULLET_WALL
} cf_event_t;

/*
==============================================================================

MEANS OF DEATH

Why somebody died. Carried in the death event and used to word the obituary.
Weapons will extend this; the environmental causes are here already because they
exist before any weapon does.

==============================================================================
*/

typedef enum {
	MOD_UNKNOWN,
	MOD_SUICIDE,
	MOD_FALLING,
	MOD_CRUSH,
	MOD_TELEFRAG,
	MOD_TRIGGER_HURT,
	MOD_WORLD,

	// One entry per weapon, so the obituary can name what did it. Append only:
	// these go over the wire in the death event.
	MOD_G17
} meansOfDeath_t;

/*
==============================================================================

COMBAT TUNING

As with the movement block above: the numbers that decide how combat feels,
kept together so they can be read as a set.

==============================================================================
*/

#define CF_MAX_HEALTH         100
#define CF_FALL_DAMAGE_SPEED  700.0f   // impact speed at which falling starts to hurt
#define CF_FALL_DAMAGE_FATAL  1000.0f  // ...and at which it kills outright

#endif // CF_SHARED_H
