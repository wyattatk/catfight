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

// "<redRoundsWon> <blueRoundsWon> <roundNumber> <roundLimit> <maxRounds>
//  <lastRoundWinner>"
//
// roundLimit, then maxRounds, then lastRoundWinner were appended later; a
// reader that stops after three fields still parses correctly and simply does
// not know what the score is out of. Append here, never insert.
//
// maxRounds is what turns a round number into a position in a match: "round 2"
// says nothing on its own, "round 2 of 3" says the whole thing. The companion
// reads both, which is why the cap had to come down the wire at all -- it was
// previously known only to the server.
//
// lastRoundWinner is the team_t that took the round that just finished, or
// TEAM_FREE for a draw, or -1 when no round has been decided yet. It exists
// because the round-end banner had no honest way to say who won: it guessed
// from whether the reader was still alive, so a player who died while his
// companion won it for him was told he had lost a round the scoreboard
// simultaneously showed him winning. Alive is not the same question as won,
// and in 2v2 they come apart every time somebody clutches.
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
	STAT_RECOIL,

	/*
	Your teammate's condition, so the HUD can show it beside your own.

	The companion is meant to be mechanically load-bearing rather than
	decoration, and you cannot make decisions about somebody whose state you
	cannot see -- "is she about to go down" is the question that decides whether
	you push or fall back, and it has to be answerable without turning round to
	look at her.

	It lives in the player's own stats rather than being read off her entity
	because an entity only reaches a client while it is in the PVS. A teammate
	fighting in the next room is exactly when this matters most, and is exactly
	when her entity is not being sent. stats are part of your own playerState
	and always arrive.

	STAT_ALLY_CLIENT is the client number PLUS ONE, with 0 meaning "no
	teammate". The offset is there so the absent case is zero rather than -1:
	stats cross the wire through MSG_WriteShort and a positive sentinel is one
	less thing to get wrong.

	Pmove never touches these, so prediction carries them through untouched.
	*/
	STAT_ALLY_CLIENT,
	STAT_ALLY_HEALTH,
	STAT_ALLY_MAX_HEALTH,

	/*
	The orders your companion is currently under, packed by CF_ORDERS_PACK.

	Here for the same reason her health is, and it is the more important of the
	two. A command system whose state the player cannot see makes every
	misunderstanding look identical to a bug -- he says "hold fire", she shoots,
	and he has no way to tell whether she misheard him, whether the order never
	arrived, or whether she is doing exactly what he asked and he misremembered
	asking. Showing the current slots answers all three at a glance, and it is
	what makes the eventual model debuggable by the person using it.
	*/
	STAT_ALLY_ORDERS,

	/*
	Your teammate's hit rate this match, 0..100.

	Here rather than derived on the client because the client has no way to
	derive it: PERS_SHOTS and PERS_HITS are in HER playerState, and a
	playerState only ever reaches the client it belongs to. This is the same
	argument as her health three fields up, applied to the one number that
	turns "how is she doing" from a guess into an answer.

	A percentage rather than the two counters, because two more shorts on the
	wire to reconstruct one number nobody wants unrounded is a poor trade.
	*/
	STAT_ALLY_ACCURACY
} statIndex_t;

typedef enum {
	PERS_SCORE,       // rounds this player was alive at the end of
	PERS_KILLS,
	PERS_DEATHS,
	PERS_TEAM,        // team_t -- so the local client can colour its own HUD
	PERS_ELIMINATED,  // out of the current round

	/*
	Trigger pulls that spent a round, and the ones that hit somebody.

	Persistent rather than per-life on purpose: accuracy over a MATCH is the
	number that means something, and one that reset every time you died would
	mostly report how long you have been alive.

	Counted in G_FireWeapon, which is the only place in the game where a shot
	exists -- pmove decides that firing happened, that function decides what it
	met, and both the player and the companion reach it through the same
	BUTTON_ATTACK path.
	*/
	PERS_SHOTS,
	PERS_HITS
} persEnum_t;

/*
Hit rate as a percentage, 0..100.

No shots reads as zero rather than dividing by it, and the caller is expected
to decide whether the sample is big enough to be worth saying out loud -- one
lucky shot is not "100% accuracy", it is one shot, and a companion who reports
it as the former sounds like a spreadsheet with a grudge.
*/
static ID_INLINE int CF_Accuracy( int shots, int hits ) {
	if ( shots <= 0 ) {
		return 0;
	}
	return ( hits * 100 ) / shots;
}

/*
Below this, a hit rate is noise rather than a fact about how somebody is
playing, and she should not mention it.

Raised from ten. Ten shots is one exchange: the difference between 30% and 40%
there is a single bullet, and she was reporting that swing as though it were a
change in how he plays -- with a number in front of her and an opinion to have
about it, which is the combination that turned her into a nag. Twenty is a full
magazine and then some, which is about the smallest sample where the second
digit is telling you anything.
*/
#define CF_ACCURACY_MIN_SHOTS  20

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

/*
Footsteps.

WALKING AND CROUCHING ARE SILENT, AND THAT IS A DESIGN DECISION rather than a
shortcut. It is what gives the +speed key a reason to exist: until now walking
was strictly worse than running, so nothing was ever gained by holding it. A
step somebody else can hear is the cost of moving fast, and choosing to give
that up is the oldest real decision in the genre.

Change it by deleting the two early returns in PM_Footsteps; the rest of the
mechanism does not care.

The cadence is Q3's proven bob-cycle scheme: an accumulator that wraps at 256
and fires on each half, so two steps per cycle. CF_STEP_BOB_RUN is per
millisecond at full running speed -- 0.4 gives a shade over three steps a
second, which is a run rather than a sprint.
*/
#define CF_STEP_BOB_RUN       0.4f
#define CF_STEP_MIN_SPEED     80.0f    // below this you are shuffling, not walking

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
#define EF_DUCKED      0x00000004   // crouched; PMF_DUCKED, which only the owner gets

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

/*
Starting and maximum health.

300 IS A TESTING VALUE, NOT A BALANCE DECISION. The real number is 100, which
is what the Glock's 26 damage was tuned against -- four shots close, six at
distance. Tripling it makes a fight last long enough to watch: the whole point
of the companion work is judging whether her aiming, shooting and dodging look
right, and at four shots a fight is over before there is anything to see.

Put this back to 100 before any judgement about how combat FEELS, and before
anything ships. Fall damage scales off it deliberately (G_CheckFallDamage), so
a fatal drop stays fatal at either number.
*/
#define CF_MAX_HEALTH         300
#define CF_FALL_DAMAGE_SPEED  700.0f   // impact speed at which falling starts to hurt

/*
THE MOMENT AFTER A DEATH, in ms: how long a player stays dead where they fell
before they become a spectator (in a round) or get back up (in warmup). The
server holds them PM_DEAD for this long, and cgame spends it on the death
camera -- your own body going down, then a turn toward whoever did it. Shared
so the camera's pacing and the server's timer cannot disagree.
*/
#define CF_DEATH_BEAT_MS      2500
#define CF_FALL_DAMAGE_FATAL  1000.0f  // ...and at which it kills outright

/*
==============================================================================

COMPANION BOTS

Tuning for the companion's combat behaviour. See code/cf_game/g_botai.c for
what each one does; they live here with the rest of the gameplay numbers rather
than buried in that file so they read as a set.

==============================================================================
*/

#define CF_BOT_TURN_SPEED     400.0f   // degrees/second the aim can swing
#define CF_BOT_REACTION       260      // ms between seeing somebody and shooting
#define CF_BOT_AIM_CONE       6.0f     // degrees of aim error still worth firing through
#define CF_BOT_SHOT_MIN       220      // ms between shots...
#define CF_BOT_SHOT_JITTER    240      // ...plus up to this, so the cadence is not a metronome
#define CF_BOT_MEMORY         1500     // ms a lost enemy is still fought

/*
How long she keeps aiming wrong in the same direction -- see G_BotApplyAimError.
The SIZE of the error is the cvar cf_botAim; this is only its pacing.

Long enough that a burst lands in roughly one place, which is what makes a miss
read as her being off rather than as the gun being unreliable. Short enough that
she corrects within a fight, so a player who stands still is not safe forever.
Jittered so two companions do not drift in step with each other.
*/
#define CF_BOT_AIM_DRIFT_MIN    600    // ms an aim error is held...
#define CF_BOT_AIM_DRIFT_JITTER 500    // ...plus up to this
#define CF_BOT_DODGE_MIN      260      // ms a sidestep runs before it may reverse...
#define CF_BOT_DODGE_JITTER   320      // ...plus up to this
#define CF_BOT_DODGE_PROBE    56.0f    // units of clearance a sidestep needs
#define CF_BOT_LEDGE_DROP     72.0f    // a bigger drop than this is a ledge, not a step

/*
Navigation -- see code/cf_game/g_botnav.c.

The two follow radii are deliberately different and that is the whole trick to
making following look calm. One radius means she starts and stops moving on the
same threshold, so at rest she is always exactly on the line where the answer
flips, and she twitches in and out of a step forever. Setting out only once the
player is further than START and stopping once inside STOP leaves a band she can
sit still in.

STOP is also kept comfortably above the 32-unit player width so she settles
beside the player rather than inside them.
*/
/*
What a companion has been told to do with her feet.

This is the movement slot of the command vocabulary, declared here rather than
in g_local.h because the HUD will have to show it -- a player who cannot see
which order is in force cannot tell a companion who misunderstood from one who
is stuck, and those need completely different reactions from him.

The whole set is implemented. It is small, closed and orthogonal on purpose:
anything that sounds like a new kind of movement should turn out to be another
value here, because every one of them resolves to a single goal point, which is
the whole reason g_botnav.c takes a point and not an order.

Where each point comes from is G_BotGoal's business and nothing else's.
*/
typedef enum {
	BOT_ORDER_FOLLOW,       // stay near the player she belongs to
	BOT_ORDER_HOLD,         // stay where she is
	BOT_ORDER_PUSH,         // advance on the last sighting, or the enemy's ground
	BOT_ORDER_FALLBACK,     // withdraw to the nearest of our own ground
	BOT_ORDER_GOTO,         // a point the player indicated, via `cf_cmd move there`

	BOT_ORDER_COUNT
} botOrder_t;

/*
When she may pull the trigger.

RETURN is the middle value and the one that justifies having three rather than
two. "Hold fire until they commit, then defend yourself" is a real thing a
player wants and it cannot be expressed as either extreme -- HOLD gets her
killed standing still, FREE gives the ambush away.
*/
typedef enum {
	BOT_ENGAGE_FREE,        // shoot anything she can see
	BOT_ENGAGE_RETURN,      // only once somebody has shot her
	BOT_ENGAGE_HOLD,        // not at all

	BOT_ENGAGE_COUNT
} botEngage_t;

/*
Who she prefers to shoot.

A preference, never a restriction, and that distinction is the whole design of
this slot. "Focus the player" while the player is behind a wall must not mean
standing there being shot by the bot -- so every value falls back to the nearest
visible enemy when its preference is not available. A companion who stops
fighting because of an order the player gave thirty seconds ago is a companion
the player learns not to give orders to.

In a 2v2 there are only two enemies, so this is nearly a binary choice -- which
is what makes it tractable here and would not be in a 5v5.
*/
typedef enum {
	BOT_FOCUS_AUTO,         // nearest visible, the default
	BOT_FOCUS_THIS,         // a specific client the player indicated
	BOT_FOCUS_PLAYER,       // prefer the human
	BOT_FOCUS_BOT,          // prefer their companion

	BOT_FOCUS_COUNT
} botFocus_t;

/*
Her three slots packed into one stat, plus whether she is stuck.

One stat rather than three because stats cross the wire individually and this is
a HUD readout, not something pmove predicts -- there is nothing to gain from
spending three of sixteen slots on it. The values are tiny and fixed-width, so
the packing is a shift and stays well inside the 16 bits a stat is sent as.

The STUCK bit is not decoration: it is how the player finds out she cannot carry
out the order he gave. Without it a companion jammed in a corner is
indistinguishable from one that ignored him.
*/
#define CF_ORDERS_PACK( move, engage, focus, stuck ) \
	( ( (move) & 7 ) | ( ( (engage) & 3 ) << 4 ) | ( ( (focus) & 3 ) << 8 ) \
	  | ( (stuck) ? ( 1 << 12 ) : 0 ) )

#define CF_ORDERS_MOVE( v )    ( (v) & 7 )
#define CF_ORDERS_ENGAGE( v )  ( ( (v) >> 4 ) & 3 )
#define CF_ORDERS_FOCUS( v )   ( ( (v) >> 8 ) & 3 )
#define CF_ORDERS_STUCK( v )   ( ( (v) >> 12 ) & 1 )

#define CF_BOT_STEP_PROBE       56.0f  // units ahead a step is checked for clearance
#define CF_BOT_FOLLOW_START    220.0f  // start following once the player is this far off
#define CF_BOT_FOLLOW_STOP     110.0f  // ...and stop once back within this
#define CF_BOT_STUCK_TIME       1200   // ms of no progress before steering admits defeat
#define CF_BOT_PROGRESS_EPSILON  8.0f  // units of gain that count as having got closer

/*
Push and fall back -- see G_BotGoal.

PUSH_MEMORY is deliberately twenty times CF_BOT_MEMORY, and they are not the
same quantity wearing different numbers. CF_BOT_MEMORY governs SHOOTING, where a
position a second and a half old is already a wasted magazine. This governs
WALKING SOMEWHERE, where a contact half a minute old is still the best
information anybody on the map has. Cutting it to CF_BOT_MEMORY would make
`push` almost always resolve to the spawn fallback instead, which looks like the
order ignoring the fight that just happened.

The arrive radii differ because the orders mean different things on arrival.
Pushing stops at fighting distance -- walking onto the spot someone was last
seen standing is how she ends up in the open with nothing to shoot at. Falling
back means actually getting there.
*/
#define CF_BOT_PUSH_MEMORY     30000   // ms a sighting is still worth advancing on
#define CF_BOT_PUSH_ARRIVE     192.0f  // units short of the push point she settles
#define CF_BOT_FALLBACK_ARRIVE  96.0f  // units short of our ground that count as back

/*
"Go there" -- see G_ResolveCrosshairPoint.

ARRIVE is much tighter than the follow radii because the player pointed at a
SPOT. Following is "be near me" and wants a band to settle in; this is "stand
about here", and stopping two body-widths short of an angle he chose would be
visibly not what he asked for. It is also why this order needs no hysteresis:
the point does not move, so there is nothing to oscillate against.

MAX_DROP is what lets a player point at a wall and mean the floor under it,
without letting him point over a balcony and mean the floor three storeys down.
*/
#define CF_GOTO_MAX_RANGE     8192.0f  // how far a crosshair order can reach
#define CF_GOTO_MAX_DROP       256.0f  // how far below the aim point a floor may be
#define CF_GOTO_ARRIVE          48.0f  // units from the spot that count as standing on it

/*
Routing -- see g_botroute.c.

A waypoint is a corner she is rounding, not a place she was sent, so arriving at
one is a LOOSER test than arriving at a destination. It has to be: an order's
arrive radius means "close enough to have carried it out", and applying that to
an intermediate hop would have her aiming to stand precisely on a corner before
admitting she had rounded it -- which in a doorway is how a companion ends up
grinding along a frame instead of walking through.

Larger than CF_GOTO_ARRIVE for that reason, and still well under the follow
band so a route cannot skip a leg it has not actually reached.
*/
#define CF_ROUTE_WAYPOINT_ARRIVE 64.0f  // units from a waypoint that count as rounded

/*
How the router samples. See G_BotRouteChooseLeg.

Each direction is STEPPED outward, DIST at a time, up to STEPS times, stopping
as soon as the way is blocked. Stepping rather than sampling one distance is not
a refinement -- it is what makes the router work at all. AAS areas are far
bigger than they look: a 1024-unit room comes out as about seven of them, so a
single 96-unit probe lands back inside the area she is already standing in,
every candidate is discarded as "same area", and the router reports a straight
line for a goal behind a solid wall while AAS is answering a perfectly good
travel time for it.

DIST is therefore the resolution of the search and STEPS its reach. The product
has to exceed the distance to the area she needs NEXT, which on an open map can
be most of the map -- from a corner of cf_nav the gap in the middle wall is 825
units away. 16 x 96 = 1536 covers a 1024-unit room corner to corner, with room
to spare on purpose: the cost of overshooting is a few traces that get refused,
and the cost of undershooting is silently walking into a wall.

Reaching through a wall is not a risk: every step is traced before it is
trusted, and a blocked step abandons the whole direction -- if 256 units that
way is through a wall, so is 512.

KNOWN LIMIT, and the reason this is sampling rather than search: candidates are
only found if they lie on one of eight rays. A map large enough that the next
area is both off-ray and beyond the reach will fall back to the straight line.
The fix when that day comes is to enumerate AAS areas into a table at map load
and pick candidates by travel time directly, rather than discovering them by
walking rays -- which removes both limits at once.

LEG_TIMEOUT is NOT a re-decide interval. A leg is held until she arrives at it,
because re-deciding near an area boundary makes her oscillate between two
waypoints in opposite directions -- see the hysteresis note in
G_BotRouteChooseLeg. This is only the backstop that stops a leg which has become
impossible from being held forever, so it wants to be long enough that a normal
leg always completes first.
*/
#define CF_ROUTE_PROBE_DIRS          8  // directions sampled around her
#define CF_ROUTE_PROBE_DIST      96.0f  // spacing of the steps along each direction
#define CF_ROUTE_PROBE_STEPS        16  // how many steps out (so 1536 units of reach)
#define CF_ROUTE_LEG_TIMEOUT      4000  // ms before an unreached leg is abandoned

#endif // CF_SHARED_H
