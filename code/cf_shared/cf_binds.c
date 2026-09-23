/*
===========================================================================
catfight -- the bindable action table.

Adding an action: add a row, give it the CURRENT CF_BIND_VERSION as its
sinceVersion, and bump CF_BIND_VERSION if this is the first addition since the
last release. Adding a row without bumping means existing players never receive
it, which is precisely the bug this table exists to prevent.

The rows deliberately mirror catfight/default.cfg rather than replacing it.
default.cfg is what a brand new player gets before any of this runs, and
leaving it in place means a fault in the migration can never leave somebody
unable to move. netplay/test-binds.ps1 asserts the two agree, so the
duplication cannot drift quietly.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "cf_binds.h"
#include "../client/keycodes.h"

static const cf_bindAction_t cf_bindActions[] = {
	// id                  display              group        command                     key         since
	{ "move_forward",      "Forward",           "Movement",  "+forward",                 'w',        1 },
	{ "move_back",         "Back",              "Movement",  "+back",                    's',        1 },
	{ "move_left",         "Strafe left",       "Movement",  "+moveleft",                'a',        1 },
	{ "move_right",        "Strafe right",      "Movement",  "+moveright",               'd',        1 },
	{ "move_jump",         "Jump",              "Movement",  "+moveup",                  K_SPACE,    1 },
	{ "move_crouch",       "Crouch",            "Movement",  "+movedown",                'c',        1 },
	{ "move_walk",         "Walk",              "Movement",  "+speed",                   K_SHIFT,    1 },

	{ "combat_attack",     "Fire",              "Combat",    "+attack",                  K_MOUSE1,   1 },
	{ "combat_zoom",       "Aim",               "Combat",    "+zoom",                    K_MOUSE2,   1 },
	{ "combat_reload",     "Reload",            "Combat",    "+reload",                  'r',        1 },
	{ "combat_weapnext",   "Next weapon",       "Combat",    "weapnext",                 'e',        1 },
	{ "combat_weapprev",   "Previous weapon",   "Combat",    "weapprev",                 'q',        1 },

	/*
	The companion set. These are the ones that were silently lost to a stale
	config, and they are the reason this whole mechanism exists.

	They are also what her voice commands become -- a key and a spoken order
	emit the same `cf_cmd`, so a player who rebinds one of these is rebinding
	the same thing the model will be saying.
	*/
	{ "companion_follow",  "Follow me",         "Companion", "cf_cmd move follow",       '6',        1 },
	{ "companion_hold",    "Hold position",     "Companion", "cf_cmd move hold",         '7',        1 },
	{ "companion_free",    "Weapons free",      "Companion", "cf_cmd engage free",       '8',        1 },
	{ "companion_holdfire","Hold fire",         "Companion", "cf_cmd engage hold",       '9',        1 },
	{ "companion_return",  "Return fire only",  "Companion", "cf_cmd engage return",     '0',        1 },
	{ "companion_focus",   "Focus my target",   "Companion", "cf_cmd focus this",        'f',        1 },
	{ "companion_auto",    "Pick own targets",  "Companion", "cf_cmd focus auto",        'g',        1 },
	{ "companion_reload",  "Tell her to reload","Companion", "cf_cmd reload",            'h',        1 },

	/*
	Added at version 2, which is the whole point of the version column: every
	player who already has version 1 picks this up on their next launch without
	losing a single key they had set.
	*/
	{ "companion_goto",    "Go where I'm aiming","Companion","cf_cmd move there",        'v',        2 },

	/*
	Added at version 3: push to talk.

	Held rather than toggled, which is why it is a +command -- the engine needs
	to know exactly when speech starts and stops, and a held key says so without
	any voice detection. See code/client/cl_voice.c.

	This is the odd one out in the Companion group. Every other row emits a
	`cf_cmd` the server validates; this one opens a microphone and what comes
	back may be an order, a question, or nothing at all. It belongs here anyway,
	because from the player's side it is simply the other way to tell her
	something.

	B because it is free and it is what most voice software defaults to. NOT t,
	which is Chat, and not v, which is already "go where I'm aiming".
	*/
	{ "companion_talk",    "Talk to her",       "Companion", "+voicerecord",             'b',        3 },

	{ "ui_scores",         "Scoreboard",        "Interface", "+scores",                  K_TAB,      1 },
	{ "ui_chat",           "Chat",              "Interface", "messagemode",              't',        1 },
	{ "ui_teamchat",       "Team chat",         "Interface", "messagemode2",             'y',        1 },

	/*
	Added at version 4: fullscreen.

	Alt+Enter already toggles fullscreen and is hardcoded in CL_KeyDownEvent so
	that it can never be unbound -- this row does not replace it, it makes the
	toggle DISCOVERABLE. Until now there was no video setting anywhere in the
	game: no menu, no keybind, nothing in the bind list. A player who did not
	already know the Alt+Enter convention had only the console, and one who did
	got 640x480 for their trouble (see CL_SetFullscreen).

	F10 because it is free. NOT F11, which is already screenshotJPEG in
	default.cfg -- and the migration deliberately refuses to hand out a default
	key that is already in use, so a collision here would not have been an
	error, it would have been this row silently never reaching anybody.
	*/
	{ "ui_fullscreen",     "Fullscreen",        "Interface", "cf_fullscreen",            K_F10,      4 },

	/*
	Added at version 6: the last two move orders.

	Z and X, beside C (crouch), V (go where I'm aiming) and B (talk to her) --
	the bottom row is already where the companion lives under the left hand.

	THEY WERE 4 AND 5 AT VERSION 5 AND THAT WAS WRONG, in a way that is the
	exact failure this table exists to prevent, so it is worth recording rather
	than quietly editing away. default.cfg has bound 4 and 5 to `weapon 4` and
	`weapon 5` since before catfight had one weapon, so every existing config
	holds them. The migration then did precisely what it is designed to do --
	refused to steal a key the player already had -- delivered NOTHING, and
	bumped cf_bindVersion anyway, so it would never retry. Version 5 shipped to
	nobody; version 6 is what actually arrives.

	THE LESSON: checking this table for a free key is not enough. The keys a
	real saved config holds are a superset of it, because default.cfg binds
	things the table has never managed. Check a config, not the table.

	Without these rows the orders exist only as console commands, which for a
	vocabulary meant to be issued MID-FIGHT is the same as not existing -- and
	the feature tested perfectly without them, because cf_bot order drives both
	from rcon.
	*/
	{ "companion_push",    "Push up",           "Companion", "cf_cmd move push",         'z',        6 },
	{ "companion_fallback","Fall back",         "Companion", "cf_cmd move fallback",     'x',        6 }
};

int CF_BindActionCount( void ) {
	return (int)ARRAY_LEN( cf_bindActions );
}

const cf_bindAction_t *CF_BindAction( int index ) {
	if ( index < 0 || index >= CF_BindActionCount() ) {
		return NULL;
	}

	return &cf_bindActions[index];
}

const cf_bindAction_t *CF_BindActionById( const char *id ) {
	int i;

	if ( !id || !id[0] ) {
		return NULL;
	}

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		if ( !Q_stricmp( cf_bindActions[i].id, id ) ) {
			return &cf_bindActions[i];
		}
	}

	return NULL;
}
