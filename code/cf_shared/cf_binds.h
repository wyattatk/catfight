/*
===========================================================================
catfight -- the set of things a player can bind a key to.

The table is in cf_shared because both the menu (which will let people rebind)
and cgame (which will want to draw "press [7] to hold position" style prompts)
need the same list, and a second copy of it would drift.

WHAT THIS IS AND IS NOT. The engine already stores key bindings perfectly well:
`bind` writes them, they are saved into catfightconfig.cfg, and they come back
on the next launch. None of that needed building. What was missing is the part
that actually bit us -- the saved config begins with `unbindall`, so a bind
newly ADDED to default.cfg never reaches anybody who has played before. Their
key does nothing, silently, with no error anywhere.

So the job of this table is to make the DEFAULTS able to evolve: name every
bindable action, say which key it wants and which version of the game it
appeared in, and let the migration in ui_binds.c fill in only what a given
player is actually missing.

`id` is the stable identity and the thing any saved data should key off.
`display` and `group` are for the menu and nothing may depend on them -- same
rule as the weapon table's displayName, so that renaming an action in the UI is
never a data migration.
===========================================================================
*/

#ifndef CF_BINDS_H
#define CF_BINDS_H

/*
Bumped whenever an action is ADDED. Each action also records the version it
arrived in, so a returning player is given only what is new to them -- which is
what stops the migration from re-adding something they deliberately unbound.
*/
#define CF_BIND_VERSION 6

typedef struct {
	const char *id;           // stable; never shown, never changed
	const char *display;      // for the menu, cosmetic
	const char *group;        // for grouping in the menu, cosmetic
	const char *command;      // what the key runs

	/*
	Keynum, not a key name, because the UI API can turn a keynum into a name
	(trap_Key_KeynumToStringBuf) but offers nothing to go the other way. For
	printable keys the keynum IS the lowercase ASCII code, so these read as
	character literals; anything else uses a K_ constant from keycodes.h.
	*/
	int         defaultKey;

	int         sinceVersion; // the CF_BIND_VERSION this action first appeared in
} cf_bindAction_t;

int                    CF_BindActionCount( void );
const cf_bindAction_t *CF_BindAction( int index );
const cf_bindAction_t *CF_BindActionById( const char *id );

#endif // CF_BINDS_H
