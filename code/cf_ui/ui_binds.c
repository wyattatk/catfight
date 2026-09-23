/*
===========================================================================
catfight -- keeping key bindings, and letting the defaults change.

The engine already does the storing. `bind` sets a key, the binding is written
into catfightconfig.cfg when the config is saved, and it comes back next launch.
Nothing here re-implements any of that, and nothing here should.

WHAT THIS FIXES IS A REAL BUG THAT COST A PLAYTEST. The saved config begins with
`unbindall` and is exec'd AFTER default.cfg, so it does not merely override the
defaults -- it erases them and restores only the keys that existed when it was
written. Any bind added to default.cfg afterwards is therefore invisible to
everybody who has ever quit the game cleanly. Their key does nothing at all: no
error, no console message, nothing in the log. That is exactly how the whole
companion command set came to be dead on a machine where it had never been
touched, and it will happen again to every playtester on every update unless
something like this runs.

THE MIGRATION RULE, and each clause is there to avoid a specific way of being
obnoxious:

  - only actions NEWER than the version the player has stored are considered,
    so an action somebody deliberately unbound is never forced back on them;
  - an action whose command is already bound to some key is left alone, because
    the player may have moved it and moving it is allowed;
  - a default key already used by something else is NOT taken. The action is
    left unbound and the menu will show it that way. Stealing a key somebody
    chose is worse than a missing default they can set in ten seconds.

The result is that adding a command is a table row plus a version bump, and
every existing player picks it up on their next launch without losing anything.
===========================================================================
*/

#include "ui_local.h"
#include "../cf_shared/cf_binds.h"

/*
================
UI_KeyForCommand

The first key currently running this command, or -1.

Compared with Q_stricmp against the whole binding, so "cf_cmd move hold" does
not match "cf_cmd move follow". A prefix compare here would report the wrong
key for every companion action, since they all share a prefix.
================
*/
static int UI_KeyForCommand( const char *command ) {
	int  key;
	char binding[MAX_STRING_CHARS];

	for ( key = 0; key < MAX_KEYS; key++ ) {
		trap_Key_GetBindingBuf( key, binding, sizeof( binding ) );

		if ( binding[0] && !Q_stricmp( binding, command ) ) {
			return key;
		}
	}

	return -1;
}

int CF_BindActionKey( const cf_bindAction_t *action ) {
	if ( !action ) {
		return -1;
	}

	return UI_KeyForCommand( action->command );
}

/*
================
CF_BindActionSetKey

Put this action on this key, and take it off whatever key it was on.

Clearing the old key first is what makes rebinding feel like moving a thing
rather than copying it -- without it the action ends up on two keys and the
menu, which shows the FIRST key it finds, would appear not to have changed.

Anything else already on the target key is displaced, deliberately: the player
has just asked for this key, and refusing them silently would be worse than
telling them afterwards. A menu should say what it displaced.
================
*/
void CF_BindActionSetKey( const cf_bindAction_t *action, int keynum ) {
	int previous;

	if ( !action || keynum < 0 || keynum >= MAX_KEYS ) {
		return;
	}

	previous = UI_KeyForCommand( action->command );
	if ( previous >= 0 && previous != keynum ) {
		trap_Key_SetBinding( previous, "" );
	}

	trap_Key_SetBinding( keynum, action->command );
	CF_BindsSave();
}

void CF_BindActionClear( const cf_bindAction_t *action ) {
	int key;

	if ( !action ) {
		return;
	}

	while ( ( key = UI_KeyForCommand( action->command ) ) >= 0 ) {
		trap_Key_SetBinding( key, "" );
	}

	CF_BindsSave();
}

/*
================
CF_BindsSave

Write the config now rather than trusting the next clean exit.

A rebind the player made and then lost to a crash, or to closing the window the
way people actually close windows, is indistinguishable from the rebind not
having worked. The engine writes the config on quit anyway; this only makes the
guarantee not depend on quitting properly.
================
*/
void CF_BindsSave( void ) {
	/*
	Written explicitly, and written NOW.

	Explicitly, because the engine's own save on quit is not enough:
	Com_WriteConfiguration returns early unless an ARCHIVED CVAR changed, and a
	rebind changes no cvar at all. A player who remapped a key and then quit
	cleanly could genuinely lose it.

	And EXEC_NOW rather than EXEC_APPEND because appending puts this at the end
	of whatever is already queued -- which, if a `quit` is sitting in the buffer,
	is after the game has gone.

	CONFIG_PREFIX is where the engine's own Q3CONFIG_CFG comes from, so this
	writes the file the engine will read back rather than a second one nobody
	loads.
	*/
	trap_Cmd_ExecuteText( EXEC_NOW, "writeconfig " CONFIG_PREFIX ".cfg\n" );
}

/*
================
CF_BindsResetToDefaults

Every action back to its default key. For a "restore defaults" button.

Two passes on purpose. Clearing everything first means the second pass can never
find a default key occupied by an action that is itself about to move, which
would otherwise leave a couple of actions unbound depending on table order.
================
*/
void CF_BindsResetToDefaults( void ) {
	int                    i;
	const cf_bindAction_t *action;

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		action = CF_BindAction( i );
		while ( 1 ) {
			int key = UI_KeyForCommand( action->command );
			if ( key < 0 ) {
				break;
			}
			trap_Key_SetBinding( key, "" );
		}
	}

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		action = CF_BindAction( i );
		trap_Key_SetBinding( action->defaultKey, action->command );
	}

	trap_Cvar_Set( "cf_bindVersion", va( "%i", CF_BIND_VERSION ) );
	CF_BindsSave();
}

/*
================
CF_BindsInit

Called once at UI startup, which is after both config files have been exec'd --
that ordering is the whole point, since the saved config is what erases the
defaults.
================
*/
void CF_BindsInit( void ) {
	int                    i;
	int                    stored;
	int                    applied;
	char                   binding[MAX_STRING_CHARS];
	const cf_bindAction_t *action;

	/*
	Registered rather than merely read, and CVAR_ARCHIVE is the load-bearing
	part: an unregistered cvar reads as 0 forever and a plain Cvar_Set on one
	does not persist, so without this the migration would believe every launch
	was the player's first and hand back anything they had unbound.
	*/
	trap_Cvar_Register( NULL, "cf_bindVersion", "0", CVAR_ARCHIVE );

	stored = (int)trap_Cvar_VariableValue( "cf_bindVersion" );

	if ( stored >= CF_BIND_VERSION ) {
		return;
	}

	applied = 0;

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		action = CF_BindAction( i );

		// Not new to this player.
		if ( action->sinceVersion <= stored ) {
			continue;
		}

		// They already have it, wherever they have put it.
		if ( UI_KeyForCommand( action->command ) >= 0 ) {
			continue;
		}

		// Its default key belongs to something else. Leave it unbound rather
		// than taking a key the player chose.
		trap_Key_GetBindingBuf( action->defaultKey, binding, sizeof( binding ) );
		if ( binding[0] ) {
			continue;
		}

		trap_Key_SetBinding( action->defaultKey, action->command );
		applied++;
	}

	trap_Cvar_Set( "cf_bindVersion", va( "%i", CF_BIND_VERSION ) );

	if ( applied > 0 ) {
		trap_Print( va( "binds: added %i new default%s for this version\n",
		                applied, applied == 1 ? "" : "s" ) );
		CF_BindsSave();
	}
}

/*
================
CF_BindsConsoleCommand

`cf_binds` -- list every action, what it is bound to, and what it wants.

Here because the menu does not exist yet and this is otherwise invisible: the
whole failure this file addresses looked exactly like a working game, so being
able to ask "what does the game think my keys are" is the difference between
diagnosing it in a minute and in an hour.
================
*/
qboolean CF_BindsConsoleCommand( const char *cmd ) {
	int                    i;
	const cf_bindAction_t *action;
	char                   keyName[64];
	char                   defName[64];
	char                   arg[MAX_TOKEN_CHARS];
	char                   arg2[MAX_TOKEN_CHARS];

	/*
	`cf_bind <action> <key>` -- what the menu will eventually do, reachable
	from the console until it exists.

	The key is taken as a NAME rather than a keynum because that is what a
	person can type, and the engine's own `bind` already knows how to parse
	one -- the UI API offers keynum-to-name but nothing the other way, so
	handing the string straight back to `bind` is the only way to accept
	"SPACE" or "MOUSE2" without duplicating the engine's key table here.

	The part `bind` alone would not do is clearing the key the action was
	previously on, which is what makes this a move rather than a copy.
	*/
	if ( !Q_stricmp( cmd, "cf_bind" ) ) {
		int previous;

		trap_Argv( 1, arg,  sizeof( arg ) );
		trap_Argv( 2, arg2, sizeof( arg2 ) );

		if ( !arg[0] || !arg2[0] ) {
			trap_Print( "usage: cf_bind <action> <key>   (cf_binds lists them)\n" );
			return qtrue;
		}

		action = CF_BindActionById( arg );
		if ( !action ) {
			trap_Print( va( "cf_bind: no action '%s'\n", arg ) );
			return qtrue;
		}

		previous = CF_BindActionKey( action );
		if ( previous >= 0 ) {
			trap_Key_SetBinding( previous, "" );
		}

		// EXEC_NOW: the binding has to exist before the save below runs, and
		// appending would also put it behind anything already queued.
		trap_Cmd_ExecuteText( EXEC_NOW,
		                      va( "bind %s \"%s\"\n", arg2, action->command ) );
		CF_BindsSave();

		trap_Print( va( "%s is now on %s\n", action->display, arg2 ) );
		return qtrue;
	}

	if ( Q_stricmp( cmd, "cf_binds" ) ) {
		return qfalse;
	}

	trap_Argv( 1, arg, sizeof( arg ) );
	if ( !Q_stricmp( arg, "reset" ) ) {
		CF_BindsResetToDefaults();
		trap_Print( "binds: every action back to its default key\n" );
		return qtrue;
	}

	trap_Print( va( "bind version %i (game is at %i)\n",
	                (int)trap_Cvar_VariableValue( "cf_bindVersion" ),
	                CF_BIND_VERSION ) );

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		int key;

		action = CF_BindAction( i );
		key = CF_BindActionKey( action );

		if ( key >= 0 ) {
			trap_Key_KeynumToStringBuf( key, keyName, sizeof( keyName ) );
		} else {
			// One word, no spaces: this line is parsed by netplay/test-binds.ps1
			// and a two-word state would need the parser to know about it.
			Q_strncpyz( keyName, "UNBOUND", sizeof( keyName ) );
		}

		trap_Key_KeynumToStringBuf( action->defaultKey, defName, sizeof( defName ) );

		// Keyed on the id, not the display name. The display name is explicitly
		// cosmetic and may be reworded at any time; anything that depended on it
		// would break silently when it was.
		trap_Print( va( "  %-20s %-12s (default %-10s) %s\n",
		                action->id, keyName, defName, action->display ) );
	}

	return qtrue;
}
