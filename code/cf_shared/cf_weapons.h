/*
===========================================================================
catfight -- weapon definitions.

Shared by the game, cgame and ui modules, because all three need the same
answer: the server decides what a shot does, the client predicts the feel of
firing it, and the HUD reports what is left in the magazine. One table, three
readers, no second copy to drift.

TWO THINGS ABOUT THIS TABLE ARE DELIBERATE.

1. `displayName` is DATA, not an identifier.

   catfight uses real firearms, and the only part of that carrying any legal
   exposure is the name string -- a gun's mechanism, magazine capacity, rate of
   fire and reload procedure are facts about an object in the world and belong
   to nobody. So the name is one field, changeable at any time, and NOTHING
   else keys off it. `id` is what appears in asset paths, cvars and save data.

   Renaming "Glock 17" to "G17" or anything else is a one-line edit here that
   touches no code, no balance, no assets and no stored data. Keep it that way:
   do not put a display name in an enum, a filename, or a switch statement.

2. Everything is an integer, in fixed units.

   Angles are hundredths of a degree, times are milliseconds. Weapon timing and
   recoil are computed inside pmove, which runs on BOTH the client (predicting)
   and the server (deciding), and the two must agree exactly. Floats drift;
   integers do not.
===========================================================================
*/

#ifndef CF_WEAPONS_H
#define CF_WEAPONS_H

// How far a hitscan shot is traced. Well past any sightline the maps have, so
// range is decided by damage falloff rather than by the bullet stopping.
#define CF_SHOT_RANGE 8192

typedef enum {
	WP_NONE,
	WP_G17,

	WP_NUM_WEAPONS
} cf_weapon_t;

typedef enum {
	FIRE_SEMI,        // one shot per trigger pull
	FIRE_AUTO,        // held trigger keeps firing
	FIRE_BURST        // fixed count per pull; unused so far
} cf_fireMode_t;

// playerState_t.weaponstate
typedef enum {
	WEAPON_READY,
	WEAPON_FIRING,
	WEAPON_RELOADING,
	WEAPON_RAISING
} cf_weaponState_t;

typedef struct {
	const char     *id;            // stable: asset paths, cvars, saved data
	const char     *displayName;   // shown to players; see the note above
	const char     *chambering;    // flavour, and it reads well on the HUD

	cf_fireMode_t   fireMode;

	/*
	The floor on time between shots.

	For a semi-automatic this is NOT the gun's cyclic rate -- a Glock's action
	cycles far faster than anyone can shoot it. The real limit is the trigger
	finger, and a fast shooter splits at roughly 150ms. 100ms sits just under
	that, so the gun is never the thing holding the player back, which is what
	makes it feel like a real trigger rather than a rate limiter.
	*/
	int             fireInterval;

	int             magazine;      // rounds in a full magazine
	qboolean        chambersRound; // can hold one in the chamber as well
	int             startingReserve;

	int             damage;
	int             falloffStart;  // units; full damage inside this
	int             falloffEnd;    // units; minimum damage beyond this
	int             falloffMin;    // percent of damage at falloffEnd

	/*
	Two reload times, because a real reload has two cases and they feel
	different.

	Tactical: rounds still in the gun, so the slide never locked back. Drop the
	magazine, seat a fresh one, done -- the chambered round was never lost, and
	the gun ends up holding magazine + 1.

	Empty: the slide locked back on the last round. Same magazine change plus
	releasing the slide to chamber a round, which is the extra half second, and
	the gun ends up holding exactly a magazine.

	Getting caught doing the slow one is a real cost that real shooters plan
	around, and reproducing it is most of why reloading is interesting.
	*/
	int             reloadTactical;
	int             reloadEmpty;
	int             drawTime;

	// Recoil, in hundredths of a degree. Pitch is up; yaw is the maximum
	// random deflection to either side.
	int             recoilPitch;
	int             recoilYaw;
	int             recoilRecover; // hundredths of a degree per second
	int             recoilMax;     // ceiling on accumulated rise

	// Dispersion, hundredths of a degree. A real pistol is mechanically far
	// more accurate than the person holding it, so the base is tiny and the
	// interesting terms are what the shooter is doing.
	int             spreadBase;
	int             spreadMoving;
	int             spreadAir;
	int             spreadPerRecoil; // added per hundredth-degree of rise
} cf_weaponInfo_t;

const cf_weaponInfo_t *CF_Weapon( int weapon );

#endif // CF_WEAPONS_H
