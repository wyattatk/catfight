/*
===========================================================================
catfight -- the weapon table.

Numbers here come from the real firearm wherever the real firearm has an
answer. Where it does not -- damage against a videogame health bar -- they are
tuned, and the comment says so. The goal is that firing it feels like firing
the real thing; it is not that a person has realistic health.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "cf_shared.h"
#include "cf_weapons.h"

static const cf_weaponInfo_t cf_weapons[WP_NUM_WEAPONS] = {
	// WP_NONE -- unarmed. Spectators and anything that has not spawned yet.
	{
		"none", "", "",
		FIRE_SEMI,
		0,
		0, qfalse, 0,
		0, 0, 0, 100,
		0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0
	},

	/*
	WP_G17 -- Glock 17.

	Chosen as the first weapon because it is the least complicated common
	pistol there is: striker-fired, no manual safety, no decocker, no hammer,
	no fire selector. Nothing about operating it needs explaining, so every
	line below is about how it SHOOTS rather than about controls.

	Real specifications used as-is:
	  - 9x19mm Parabellum
	  - 17-round magazine, and one may be carried in the chamber (17+1)
	  - semi-automatic; one shot per trigger pull, always
	  - the slide locks back on the last round, which is why an empty reload
	    costs more than a tactical one
	  - reload times are those of a practised shooter: roughly 1.6s with the
	    slide forward, roughly 2.1s from slide-lock

	Tuned rather than real:
	  - damage. 26 is four shots to a 100-health player. A real 9mm is not
	    four-shots-to-stop, but player health is a game rule and the user's
	    call was explicitly that health need not be realistic while the GUN
	    should be.
	  - falloff. A pistol round genuinely loses energy with distance, and the
	    numbers here express "this is a close-range weapon" rather than any
	    particular ballistic table.
	*/
	{
		"g17", "Glock 17", "9x19mm",
		FIRE_SEMI,
		100,                     // trigger-limited, not action-limited
		17, qtrue, 68,           // 17+1 in the gun, four spare magazines
		26, 1200, 3000, 55,      // four shots close, six at distance
		1600, 2100, 500,         // tactical / empty / draw

		/*
		Recoil. A 9mm Glock is a mild gun: the muzzle flips up, snaps back
		down, and does not wander much. 85 hundredths of a degree per shot
		with a 250/second recovery means a deliberate shooter is back on
		target before they can pull the trigger again, while emptying the
		magazine as fast as possible walks the sights up the target -- which
		is exactly what happens on a range.

		The yaw figure is small and random. Real recoil is not perfectly
		vertical, but a pistol does not have a memorised spray pattern
		either, and pretending it does would be a different game.
		*/
		85, 22, 250, 700,

		/*
		Dispersion. The base is deliberately almost nothing: a Glock in a
		vice will put every round through one hole, and any inaccuracy the
		player feels should be something THEY did. Moving, being airborne and
		having just fired are all the shooter, not the gun.
		*/
		4, 90, 320, 3
	}
};

const cf_weaponInfo_t *CF_Weapon( int weapon ) {
	if ( weapon < 0 || weapon >= WP_NUM_WEAPONS ) {
		return &cf_weapons[WP_NONE];
	}
	return &cf_weapons[weapon];
}
