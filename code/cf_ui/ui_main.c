/*
===========================================================================
catfight -- user interface module.

The engine hard-requires a UI module -- it refuses to start without one, and it
hands the UI control of the screen whenever the client is not in a level. So
this file owns the screen at startup, during connection, and whenever the player
presses escape.

The home screen is built out of one button primitive and reads matchmaking state
straight from the `mm_state` and `mm_statusText` cvars. That is the whole reason
Phase 3 put matchmaking in the engine and exposed it as cvars rather than adding
UI syscalls: a menu that can read a cvar needs no new engine surface at all, so
none was added for this.

Everything is laid out in a virtual 640x480 and scaled, so one set of numbers
works at every resolution. There is still no art and no font beyond the console
charset -- the menu is drawn from filled rectangles on purpose, and is meant to
be replaced wholesale once there is a real look.
===========================================================================
*/

#include "ui_local.h"
#include "../cf_shared/cf_binds.h"

uiStatic_t uis;

typedef enum {
	MENU_NONE,
	MENU_MAIN,     // not connected -- we own the whole screen
	MENU_INGAME,   // connected, escape pressed -- we overlay the world
	MENU_STATS,    // everything she remembers, for a playtester to read
	MENU_CONTROLS, // every key the game listens to, on one page
	MENU_AUDIO     // volumes, and which box the sound comes out of
} cfMenu_t;

static cfMenu_t uiMenu;

// The stats page. Up here rather than beside its own screen because escape
// handling has to be able to disarm the reset, and that runs long before.
static int      uiStatsScroll;
static qboolean uiResetArmed;

// The controls page, scrolled separately from the stats page: they are
// different lengths, and sharing one offset means opening the short one after
// the long one lands you off the bottom of it.
//
// Up here with the counter rather than beside the screen, because UI_Internal
// pages both lists and runs long before either is drawn.
#define CONTROLS_VISIBLE_LINES 21
static int      uiControlsScroll;

// Which screen the audio page returns to. Up here with the other menu state
// because UI_Internal sets it long before the audio screen is defined.
static qboolean uiAudioFromGame;

/*
Click-to-rebind.

uiCaptureAction is the index into the bind table of the action waiting for a
key, or -1 when nothing is. While it is set, UI_KeyEvent stops being menu
navigation entirely and the NEXT key press becomes the binding -- which is the
only way a controls page can let somebody bind ENTER, TAB or an arrow, all of
which the menu would otherwise eat as navigation.

uiCaptureNote is what happened, kept on screen afterwards. Rebinding is the one
action on this page with an invisible side effect: putting an action on a key
that something else already had takes it away from that other thing, silently,
and the player will not scroll back up to notice. CF_BindActionSetKey's own
comment says a menu should say what it displaced; this is the menu saying it.

The rows a click can land on are recorded during the draw, because only the
draw knows which of the thirty-three rows are on screen and where the scroll
put them.
*/
typedef struct {
	float y;
	int   action;   // index into the bind table
} uiControlRow_t;

static uiControlRow_t uiControlRows[CONTROLS_VISIBLE_LINES];
static int            uiNumControlRows;
static int            uiCaptureAction = -1;
static char           uiCaptureNote[128];

static const float colorBackground[4] = { 0.055f, 0.06f, 0.075f, 1.0f };
static const float colorOverlay[4]    = { 0.0f, 0.0f, 0.0f, 0.55f };
static const float colorClaw[4]       = { 0.95f, 0.62f, 0.25f, 1.0f };
static const float colorClawDim[4]    = { 0.35f, 0.25f, 0.15f, 1.0f };
static const float colorText[4]       = { 0.85f, 0.86f, 0.88f, 1.0f };
static const float colorTextDim[4]    = { 0.45f, 0.47f, 0.52f, 1.0f };
static const float colorButton[4]     = { 0.10f, 0.11f, 0.13f, 1.0f };
static const float colorButtonHot[4]  = { 0.17f, 0.15f, 0.13f, 1.0f };
static const float colorError[4]      = { 0.92f, 0.35f, 0.30f, 1.0f };
static const float colorShadow[4]     = { 0.0f, 0.0f, 0.0f, 0.85f };

/*
The menu is immediate mode: each screen rebuilds its buttons every frame, and
the list that was built last frame is what a click tests against. That keeps
the screens declarative -- there is no widget tree to keep in sync with the
matchmaking state, which changes underneath us without asking.
*/
// Six was exactly enough until the home screen grew an AUDIO button beside
// QUIT, which with a result card up makes six on one screen. Eight leaves room
// to be wrong about that again.
#define MAX_MENU_BUTTONS 8

typedef struct {
	float x, y, w, h;
	char  label[32];
	char  cmd[64];
} uiButton_t;

static uiButton_t uiButtons[MAX_MENU_BUTTONS];
static int        uiNumButtons;
static int        uiSelected;   // highlighted button, -1 for none

/*
================
Com_Printf / Com_Error

q_shared.c is engine code compiled into every module and reports problems
through these; inside a module they have to route back out through the syscall
interface.
================
*/
void QDECL Com_Printf( const char *msg, ... ) {
	va_list argptr;
	char    text[1024];

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );

	trap_Print( text );
}

void QDECL Com_Error( int level, const char *error, ... ) {
	va_list argptr;
	char    text[1024];

	(void)level;

	va_start( argptr, error );
	Q_vsnprintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

	trap_Error( text );
}

static void QDECL UI_Printf( const char *msg, ... ) Q_PRINTF_FUNC( 1, 2 );

static void QDECL UI_Printf( const char *msg, ... ) {
	va_list argptr;
	char    text[1024];

	va_start( argptr, msg );
	Q_vsnprintf( text, sizeof( text ), msg, argptr );
	va_end( argptr );

	trap_Print( text );
}

/*
================
UI_FillRect

Laid out in a virtual 640x480 and scaled to the window, so the same numbers
mean the same thing at any resolution.
================
*/
static void UI_FillRect( float x, float y, float w, float h, const float *color ) {
	trap_R_SetColor( color );

	x *= uis.xscale;
	y *= uis.yscale;
	w *= uis.xscale;
	h *= uis.yscale;

	trap_R_DrawStretchPic( x, y, w, h, 0, 0, 0, 0, uis.white );

	trap_R_SetColor( NULL );
}

/*
================
UI_DrawString

The font sheet is a 16x16 grid of cells indexed by byte value, the same layout
the engine's own console drawing uses (cl_scrn.c). charWidth is normally about
half charHeight, which is the aspect the glyphs are drawn at.
================
*/
static void UI_DrawString( float x, float y, const char *s, float charWidth, float charHeight,
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

			ax = x * uis.xscale;
			ay = y * uis.yscale;
			aw = charWidth * uis.xscale;
			ah = charHeight * uis.yscale;

			trap_R_DrawStretchPic( ax, ay, aw, ah,
			                       fcol, frow, fcol + 0.0625f, frow + 0.0625f, uis.charset );
		}

		x += charWidth;
		s++;
	}

	trap_R_SetColor( NULL );
}

static float UI_StringWidth( const char *s, float charWidth ) {
	return strlen( s ) * charWidth;
}

static void UI_DrawStringCentred( float cx, float y, const char *s, float charWidth,
                                  float charHeight, const float *color ) {
	UI_DrawString( cx - UI_StringWidth( s, charWidth ) * 0.5f, y, s, charWidth, charHeight, color );
}

/*
================
UI_DrawSlash

A diagonal line, drawn as a run of small squares. There is no line primitive in
the 2D renderer and no art to use instead, so this is how catfight draws its
one piece of identity until there is a real one.
================
*/
static void UI_DrawSlash( float x0, float y0, float x1, float y1, float thickness,
                          const float *color ) {
	const int steps = 48;
	int       i;
	float     t, x, y;

	for ( i = 0; i <= steps; i++ ) {
		t = (float)i / steps;
		x = x0 + ( x1 - x0 ) * t;
		y = y0 + ( y1 - y0 ) * t;
		UI_FillRect( x - thickness * 0.5f, y - thickness * 0.5f, thickness, thickness, color );
	}
}

/*
================
UI_DrawClawMark

Three slashes. Deliberately not a logo -- a placeholder that is at least ours.
================
*/
static void UI_DrawClawMark( float cx, float cy, float scale, const float *color ) {
	int   i;
	float dx;

	for ( i = -1; i <= 1; i++ ) {
		dx = i * 26.0f * scale;
		UI_DrawSlash( cx + dx - 18 * scale, cy - 40 * scale,
		              cx + dx + 12 * scale, cy + 40 * scale,
		              5.0f * scale, color );
	}
}

/*
================
UI_DrawCursor

The engine puts the mouse in relative mode and hides the OS pointer while a menu
is up, so if we do not draw a cursor there is none. A filled triangle, with a
dark copy underneath it so it stays visible against the light parts of a button.
================
*/
static void UI_DrawCursor( void ) {
	const int rows = 14;
	int       i;
	float     w;

	for ( i = 0; i < rows; i++ ) {
		w = 1.0f + i * 0.62f;
		UI_FillRect( uis.cursorx + 1, uis.cursory + i + 1, w + 1, 1, colorShadow );
	}
	for ( i = 0; i < rows; i++ ) {
		w = 1.0f + i * 0.62f;
		UI_FillRect( uis.cursorx, uis.cursory + i, w, 1, colorText );
	}
}

// ---------------------------------------------------------------------------
// buttons
// ---------------------------------------------------------------------------

static void UI_BeginButtons( void ) {
	uiNumButtons = 0;
}

static void UI_AddButton( float x, float y, float w, float h,
                          const char *label, const char *cmd ) {
	uiButton_t *b;

	if ( uiNumButtons >= MAX_MENU_BUTTONS ) {
		return;
	}

	b = &uiButtons[uiNumButtons++];
	b->x = x;
	b->y = y;
	b->w = w;
	b->h = h;
	Q_strncpyz( b->label, label, sizeof( b->label ) );
	Q_strncpyz( b->cmd, cmd, sizeof( b->cmd ) );
}

static int UI_ButtonUnderCursor( void ) {
	int i;

	for ( i = 0; i < uiNumButtons; i++ ) {
		const uiButton_t *b = &uiButtons[i];

		if ( uis.cursorx >= b->x && uis.cursorx < b->x + b->w &&
		     uis.cursory >= b->y && uis.cursory < b->y + b->h ) {
			return i;
		}
	}
	return -1;
}

static void UI_DrawButton( const uiButton_t *b, qboolean hot ) {
	const float *edge = hot ? colorClaw : colorClawDim;
	const float *face = hot ? colorButtonHot : colorButton;
	const float *ink  = hot ? colorClaw : colorText;

	UI_FillRect( b->x, b->y, b->w, b->h, face );

	// a one-pixel frame, drawn as four fills -- there is no outline primitive
	UI_FillRect( b->x, b->y, b->w, 1, edge );
	UI_FillRect( b->x, b->y + b->h - 1, b->w, 1, edge );
	UI_FillRect( b->x, b->y, 1, b->h, edge );
	UI_FillRect( b->x + b->w - 1, b->y, 1, b->h, edge );

	// a thicker bar on the selected edge, so the highlight survives being read
	// at a glance and does not depend on colour alone
	if ( hot ) {
		UI_FillRect( b->x, b->y, 3, b->h, colorClaw );
	}

	UI_DrawStringCentred( b->x + b->w * 0.5f, b->y + b->h * 0.5f - 8, b->label, 10, 18, ink );
}

static void UI_DrawButtons( void ) {
	int i;

	for ( i = 0; i < uiNumButtons; i++ ) {
		UI_DrawButton( &uiButtons[i], i == uiSelected );
	}
}

/*
================
UI_ActivateButton

A button's command is a console command, except when it starts with '!'.

The escape hatch exists because some buttons act on the MENU rather than on the
game -- which screen is showing, how far down a page has been scrolled -- and
routing those through the command buffer would mean inventing console commands
for things the console has no business knowing about. It would also make them
asynchronous: Cbuf_AddText runs the command next frame, which is fine for
"connect me to a server" and wrong for "scroll down".
================
*/
static void UI_Internal( const char *cmd );

// The controls page lives much further down, but the key and mouse handlers sit
// up here with the rest of the input and have to reach it.
static void     UI_CaptureKey( int key );
static qboolean UI_ControlsClick( void );
static qboolean UI_AudioClick( void );
static void     UI_AudioSync( void );
static void     UI_AudioApply( void );

static void UI_ActivateButton( int index ) {
	if ( index < 0 || index >= uiNumButtons ) {
		return;
	}
	if ( !uiButtons[index].cmd[0] ) {
		return;
	}
	if ( uiButtons[index].cmd[0] == '!' ) {
		UI_Internal( uiButtons[index].cmd + 1 );
		return;
	}
	trap_Cmd_ExecuteText( EXEC_APPEND, va( "%s\n", uiButtons[index].cmd ) );
}

static void UI_CentreCursor( void ) {
	uis.cursorx = 320;
	uis.cursory = 240;
	uiSelected  = -1;
}

/*
================
UI_MatchmakingState

mm_state is one of idle / searching / found / error, written by the engine's
matchmaking client (cl_mm.c). Treat anything unrecognised as idle so a future
state cannot strand the player on a screen with no way out.
================
*/
static void UI_MatchmakingState( char *state, int stateSize, char *text, int textSize ) {
	trap_Cvar_VariableStringBuffer( "mm_state", state, stateSize );
	trap_Cvar_VariableStringBuffer( "mm_statusText", text, textSize );

	if ( !state[0] ) {
		Q_strncpyz( state, "idle", stateSize );
	}
}

/*
================
UI_LastResult

How the last match went, written by cgame at MATCH_END and left in cvars
because they are the only thing that survives the disconnect between the
module that knew the result and this one, which has to show it.

cf_lastResult is "win" / "loss" / "draw" / "over"; cf_lastScore is the score
with the player's own side first. Both are empty when there is nothing to show,
which is the state cgame puts them back into as the next match begins.
================
*/
static qboolean UI_LastResult( char *result, int resultSize, char *score, int scoreSize ) {
	trap_Cvar_VariableStringBuffer( "cf_lastResult", result, resultSize );
	trap_Cvar_VariableStringBuffer( "cf_lastScore", score, scoreSize );

	return (qboolean)( result[0] != '\0' );
}

// clears both, as one console command, for the DISMISS button
#define UI_CLEAR_RESULT "set cf_lastResult \"\"; set cf_lastScore \"\""

static void UI_DrawResultCard( const char *result, const char *score ) {
	const char  *headline;
	const float *color;

	if ( !Q_stricmp( result, "win" ) ) {
		headline = "VICTORY";
		color = colorClaw;
	} else if ( !Q_stricmp( result, "loss" ) ) {
		headline = "DEFEAT";
		color = colorError;
	} else if ( !Q_stricmp( result, "draw" ) ) {
		headline = "DRAW";
		color = colorText;
	} else {
		headline = "MATCH OVER";
		color = colorTextDim;
	}

	UI_DrawStringCentred( 320, 238, headline, 14, 28, color );

	if ( score[0] ) {
		UI_DrawStringCentred( 320, 272, score, 10, 18, colorTextDim );
	}
}

/*
================
UI_CheckPlayAgain

The postgame's PLAY AGAIN, finished.

cgame cannot queue and disconnect in one go -- `disconnect` raises
ERR_DISCONNECT, which longjmps out of the command buffer and throws away
anything behind it, so "disconnect; mm_find" would silently do only the first
half. It leaves the intent in cf_playAgain instead, and this is the other end:
the menu is up, the client is idle, and the search can actually start.
================
*/
static void UI_CheckPlayAgain( const char *state ) {
	char again[8];

	trap_Cvar_VariableStringBuffer( "cf_playAgain", again, sizeof( again ) );

	if ( !atoi( again ) ) {
		return;
	}

	// cleared first, whatever happens next: a flag that survived its own
	// handling would re-queue the player every time they came back to the menu
	trap_Cvar_Set( "cf_playAgain", "0" );

	// Anything other than idle means the engine is already busy with a search
	// or a connection of its own, and mm_find would only be refused.
	if ( Q_stricmp( state, "idle" ) ) {
		return;
	}

	trap_Cmd_ExecuteText( EXEC_APPEND, "mm_find\n" );
}

/*
================
UI_SetActiveMenu

The engine tells us which menu should be up. UIMENU_MAIN arrives every frame
while the client is disconnected, so this has to be cheap and idempotent.
================
*/
static void UI_SetActiveMenu( uiMenuCommand_t menu ) {
	switch ( menu ) {
	case UIMENU_NONE:
		uiMenu = MENU_NONE;
		trap_Key_SetCatcher( trap_Key_GetCatcher() & ~KEYCATCH_UI );
		trap_Key_ClearStates();
		trap_Cvar_Set( "cl_paused", "0" );
		return;

	case UIMENU_MAIN:
		/*
		The stats screen is a page BEHIND the home screen, not a menu the
		engine knows about -- and UIMENU_MAIN arrives every frame while
		disconnected. Without this it would be closed again one frame after it
		opened, which looks exactly like the button not working.
		*/
		if ( uiMenu == MENU_STATS || uiMenu == MENU_CONTROLS
		     || uiMenu == MENU_AUDIO ) {
			trap_Key_SetCatcher( KEYCATCH_UI );
			return;
		}

		// arrives every frame while disconnected, so only the transition into
		// the menu may move the cursor -- otherwise it would be pinned to the
		// middle and never move at all
		if ( uiMenu != MENU_MAIN ) {
			UI_CentreCursor();
		}
		uiMenu = MENU_MAIN;
		trap_Key_SetCatcher( KEYCATCH_UI );
		return;

	case UIMENU_INGAME:
		if ( uiMenu != MENU_INGAME ) {
			UI_CentreCursor();
		}
		uiMenu = MENU_INGAME;
		trap_Key_SetCatcher( KEYCATCH_UI );
		return;

	default:
		// team/postgame/cd-key menus do not exist in catfight
		return;
	}
}

/*
================
UI_KeyEvent
================
*/
static void UI_KeyEvent( int key, qboolean down ) {
	if ( !down ) {
		return;
	}

	/*
	A waiting row swallows the whole keyboard, before any of the navigation
	below gets a look at it. That is the point: ENTER, TAB, SPACE and the arrows
	are all keys somebody may reasonably want to bind, and they are all keys
	this menu would otherwise consume. See UI_CaptureKey.
	*/
	if ( uiMenu == MENU_CONTROLS && uiCaptureAction >= 0 ) {
		UI_CaptureKey( key );
		return;
	}

	switch ( key ) {
	case K_ESCAPE:
		if ( uiMenu == MENU_INGAME ) {
			UI_SetActiveMenu( UIMENU_NONE );
			return;
		}

		// Out of a sub-page and back to the home screen, which is what escape
		// means everywhere else: up one, not out of the game.
		if ( uiMenu == MENU_STATS || uiMenu == MENU_CONTROLS
		     || uiMenu == MENU_AUDIO ) {
			uiMenu           = ( uiMenu == MENU_AUDIO && uiAudioFromGame )
			                   ? MENU_INGAME : MENU_MAIN;
			uiResetArmed     = qfalse;
			// Defensive: a waiting row eats escape itself (it is how you
			// cancel), so this is only reachable with nothing armed.
			uiCaptureAction  = -1;
			uiCaptureNote[0] = '\0';
			return;
		}

		/*
		On the main menu, escape backs out of matchmaking.

		Escape is the key everybody presses when a screen will not let them
		leave, so it should do the obvious thing rather than nothing. While
		idle it still means nothing -- there is nowhere to go back to from the
		top menu -- and quitting on escape would be a nasty surprise.
		*/
		if ( uiMenu == MENU_MAIN ) {
			char state[32], status[256];

			UI_MatchmakingState( state, sizeof( state ), status, sizeof( status ) );
			if ( !Q_stricmp( state, "searching" ) || !Q_stricmp( state, "found" ) ) {
				trap_Cmd_ExecuteText( EXEC_APPEND, "mm_cancel; disconnect\n" );
			}
		}
		return;

	case K_MOUSE1:
		// A row first: the list covers most of the page and the buttons sit
		// below it, so the two never overlap and the order only decides which
		// gets asked. A click that hits a row must not also press a button.
		if ( uiMenu == MENU_AUDIO && UI_AudioClick() ) {
			return;
		}
		if ( uiMenu == MENU_CONTROLS && UI_ControlsClick() ) {
			return;
		}
		UI_ActivateButton( UI_ButtonUnderCursor() );
		return;

	case K_UPARROW:
	case K_KP_UPARROW:
		if ( uiNumButtons > 0 ) {
			uiSelected = ( uiSelected <= 0 ? uiNumButtons : uiSelected ) - 1;
		}
		return;

	case K_DOWNARROW:
	case K_KP_DOWNARROW:
	case K_TAB:
		if ( uiNumButtons > 0 ) {
			uiSelected = ( uiSelected + 1 ) % uiNumButtons;
		}
		return;

	case K_ENTER:
	case K_KP_ENTER:
	case K_SPACE:
		UI_ActivateButton( uiSelected );
		return;

	default:
		// everything else is left alone -- notably ~, so the console is still
		// reachable from any screen
		return;
	}
}

/*
================
UI_MouseEvent

Relative motion, in the same units the rest of the layout uses. Clamped rather
than wrapped, and kept a little inside the edge so the cursor never leaves in a
way the player cannot undo.
================
*/
static void UI_MouseEvent( int dx, int dy ) {
	int hovered;

	uis.cursorx += dx;
	uis.cursory += dy;

	if ( uis.cursorx < 0 )   { uis.cursorx = 0; }
	if ( uis.cursorx > 639 ) { uis.cursorx = 639; }
	if ( uis.cursory < 0 )   { uis.cursory = 0; }
	if ( uis.cursory > 479 ) { uis.cursory = 479; }

	// Moving the mouse takes over the selection from the keyboard, but moving
	// off a button does not clear it -- otherwise the highlight flickers away
	// whenever the pointer crosses the gap between two buttons.
	hovered = UI_ButtonUnderCursor();
	if ( hovered >= 0 ) {
		uiSelected = hovered;
	}
}

/*
================
UI_Refresh

Draw whatever menu is up. Only called when the UI holds the key catcher.
================
*/
/*
================
UI_TalkHint

"Hold B to talk to your companion", on the menu as well as in the game.

SHE CAN BE TALKED TO HERE, and nothing said so. Push-to-talk works at the home
screen -- cl_voice.c sends her a situation describing the house precisely so
that it can -- and the between-matches lull is the best time in the whole game
to actually have a conversation with her rather than shout an order. A hint
that only existed on the HUD was missing the calmer half of the feature.

The same two cvars cgame reads, for the same reason it reads them: the engine
owns the bindings and the helper, this module owns neither, and a cvar crosses
that gap without a syscall. Silent unless she is actually up, because telling
somebody to talk to a companion who is not running is worse than saying
nothing.

Returns whether it drew, so callers can lay out what follows it.
================
*/
static qboolean UI_TalkHint( float y ) {
	char state[32], key[32];

	trap_Cvar_VariableStringBuffer( "cl_voiceState", state, sizeof( state ) );
	if ( Q_stricmp( state, "ready" ) ) {
		return qfalse;
	}

	trap_Cvar_VariableStringBuffer( "cl_voiceKey", key, sizeof( key ) );
	if ( !key[0] ) {
		// Bound to nothing. Says so rather than staying quiet -- an unbound
		// push-to-talk is invisible everywhere else, and CONTROLS is two
		// clicks away from here.
		UI_DrawStringCentred( 320, y, "No key talks to your companion -- see CONTROLS",
		                      6, 12, colorTextDim );
		return qtrue;
	}

	UI_DrawStringCentred( 320, y, va( "Hold %s to talk to your companion", key ),
	                      6, 12, colorTextDim );
	return qtrue;
}

static void UI_DrawHome( void ) {
	char  state[32], status[256], server[128];
	float pulse;
	float c[4];
	float y;

	UI_MatchmakingState( state, sizeof( state ), status, sizeof( status ) );

	UI_FillRect( 0, 0, 640, 480, colorBackground );

	// a slow pulse, so an idle main menu still reads as a running game
	pulse = 0.75f + 0.25f * sin( trap_Milliseconds() * 0.0015f );
	c[0] = colorClaw[0] * pulse;
	c[1] = colorClaw[1] * pulse;
	c[2] = colorClaw[2] * pulse;
	c[3] = 1.0f;

	UI_DrawClawMark( 320, 110, 1.15f, c );
	UI_DrawStringCentred( 320, 158, "CATFIGHT", 22, 44, colorClaw );
	UI_FillRect( 200, 220, 240, 1, colorClawDim );

	UI_BeginButtons();

	if ( !Q_stricmp( state, "searching" ) || !Q_stricmp( state, "found" ) ) {
		// mm_statusText already reads as a sentence ("searching for an opponent
		// (12s)"), so it is shown as-is rather than re-worded here.
		UI_DrawStringCentred( 320, 238, status, 8, 16, colorText );

		// a bar sliding under the status, so a long search still looks alive
		{
			float phase = ( trap_Milliseconds() % 1400 ) / 1400.0f;

			UI_FillRect( 200, 266, 240, 2, colorClawDim );
			UI_FillRect( 200 + 200 * phase, 266, 40, 2, colorClaw );
		}

		/*
		There is ALWAYS a way out, including from "found".

		"found" was treated as a blink -- the engine issues the connect the
		moment it hears back -- so this screen used to draw no buttons at all
		in that state. When the connect then did not complete, the player was
		left watching a spinner with nothing to click and no way back short of
		killing the game. That is exactly what a stuck matchmade server caused.

		A state that is *usually* instantaneous is still a state, and a screen
		with no exit is never the right answer to one.

		Both commands, because from "found" the client may already be part way
		into a connection that is going nowhere, and mm_cancel alone would leave
		it there. `mm_cancel` FIRST: `disconnect` raises ERR_DISCONNECT, which
		longjmps out of the command buffer, so anything behind it in the same
		buffer is thrown away -- in the other order the cancel never ran and the
		player was left queued on the matchmaker they thought they had left.
		Neither is harmful when there is nothing to do.
		*/
		UI_AddButton( 200, 300, 240, 40, "BACK", "mm_cancel; disconnect" );

		// Waiting in a queue is the other calm moment, and there is nothing
		// else on this screen. She knows she is queuing -- cl_voice.c tells her
		// so -- which makes this the one place the hint is also a suggestion.
		UI_TalkHint( 360 );
	} else {
		char     result[16], score[32];
		qboolean haveResult;

		haveResult = UI_LastResult( result, sizeof( result ), score, sizeof( score ) );

		/*
		A result card wins the space over a matchmaking error. They occupy the
		same band, and if both are true the error is about a search that has not
		started yet while the card is about a match that actually happened --
		which is the more useful of the two to be looking at.
		*/
		if ( haveResult ) {
			UI_DrawResultCard( result, score );
		} else if ( !Q_stricmp( state, "error" ) ) {
			UI_DrawStringCentred( 320, 238, status, 8, 16, colorError );
		}

		// PLAY is mm_find and nothing else: the engine owns the search, and it
		// reports back through the same cvars this screen already reads.
		if ( haveResult ) {
			/*
			Tighter than the idle layout below, because this variant has four
			rows under a result card rather than three and the footer now
			carries the talk hint as well. At the old 288/40/36 the column ran
			to y=444 and went straight through it.
			*/
			y = 292;
			UI_AddButton( 200, y, 240, 32, "PLAY AGAIN", "mm_find" );
			y += 36;
			// DISMISS, not BACK: there is nothing behind this screen. It only
			// retires the card, leaving the ordinary home screen.
			UI_AddButton( 200, y, 240, 32, "DISMISS", UI_CLEAR_RESULT );
			y += 36;
			// Paired on one row so adding CONTROLS costs no vertical space --
			// this column already ends about where the footer text starts.
			UI_AddButton( 200, y, 116, 32, "COMPANION", "!stats" );
			UI_AddButton( 324, y, 116, 32, "CONTROLS", "!controls" );
			y += 36;
			// AUDIO pairs with QUIT rather than squeezing onto the row above:
			// three 76px buttons fit the width but not the word COMPANION.
			UI_AddButton( 200, y, 116, 32, "AUDIO", "!audio" );
			UI_AddButton( 324, y, 116, 32, "QUIT", "quit" );
		} else {
			y = 274;
			UI_AddButton( 200, y, 240, 40, "PLAY", "mm_find" );
			y += 48;
			/*
			COMPANION rather than STATS, because what is behind it is her --
			what she remembers, how she is with this player, and the button
			that wipes both. A tester looking for "why is she like that" looks
			for her name for it, not for a word about numbers.
			*/
			UI_AddButton( 200, y, 116, 40, "COMPANION", "!stats" );
			UI_AddButton( 324, y, 116, 40, "CONTROLS", "!controls" );
			y += 48;
			UI_AddButton( 200, y, 116, 40, "AUDIO", "!audio" );
			// There used to be a PRACTICE button here, running `map cf_test`.
			// It is gone on purpose: `map` starts a listen server, which needs
			// the game module in the shipped package, and shipping that module
			// is the one thing the licensing decision says we do not do. Every
			// route into the game now goes through the pool.
			UI_AddButton( 324, y, 116, 40, "QUIT", "quit" );
		}
	}

	UI_DrawButtons();

	/*
	The footer, bottom-up so that the talk hint does not have to know whether
	the postgame card pushed the buttons down.

	The card variant runs four rows to y=444 and the idle one three to y=410,
	so a fixed y for this would either collide with the buttons or float in the
	middle of the screen depending on a state it has no business knowing about.
	Laid out from the bottom, the only thing that varies is how much empty space
	is above it.
	*/
	trap_Cvar_VariableStringBuffer( "mm_server", server, sizeof( server ) );

	UI_DrawStringCentred( 320, 466, "~ for the console", 6, 12, colorTextDim );
	if ( server[0] ) {
		UI_DrawStringCentred( 320, 452, va( "matchmaker  %s", server ), 6, 12, colorTextDim );
	}
	UI_TalkHint( 438 );   // clears the idle column at 410 and the card one at 432
}

/*
===========================================================================

THE STATS SCREEN

Everything the companion knows about this player, on one page, because during
development the people testing her are the ones who have to describe what she
did wrong -- and "she was weird about the loss" is not a bug report while
"support 0, mood -5, familiarity 2" is.

It renders the helper's own listing rather than laying out fields. That text is
already written for a person to read, it is the same thing `voice_memory`
prints, and every number on it is one the model was actually given. A screen
that re-derived any of it would eventually disagree with her, and then the
screen would be lying about the thing it exists to explain.

Temporary in the sense that it shows everything at once and says so. It is not
temporary in the sense of being sloppy: a tester who cannot trust the readout
cannot report anything with it.

===========================================================================
*/

#define STATS_VISIBLE_LINES  22
#define MAX_STATS_TEXT       ( 16 * 1024 )
/*
Characters that fit across the page at 6px, inside the margins, and the most
rows that can come out of wrapping to it.

A match she wrote up is two sentences and runs well past the screen, so these
lines are WRAPPED rather than clipped. Clipping was the first version and it
was quietly the worst possible failure for this screen: the episodes are the
part a tester most needs to read when she says something strange about a match,
and the half that fell off the right was the half that would have explained it.
*/
#define STATS_COLS           96
#define MAX_STATS_ROWS       512

static void UI_Internal( const char *cmd ) {
	if ( !Q_stricmp( cmd, "stats" ) ) {
		uiMenu        = MENU_STATS;
		uiStatsScroll = 0;
		uiResetArmed  = qfalse;
		return;
	}
	if ( !Q_stricmp( cmd, "controls" ) ) {
		uiMenu           = MENU_CONTROLS;
		uiControlsScroll = 0;
		uiResetArmed     = qfalse;
		uiCaptureAction  = -1;
		uiCaptureNote[0] = '\0';
		return;
	}
	if ( !Q_stricmp( cmd, "audio" ) ) {
		/*
		Remembered because this page is reachable from two places and BACK has
		to mean the same thing from both. Opened from the pause menu, returning
		to the home screen would be indistinguishable from being thrown out of
		the match -- and the only button there that leaves a match is one the
		player deliberately did not press.
		*/
		uiAudioFromGame = ( uiMenu == MENU_INGAME );
		uiMenu       = MENU_AUDIO;
		uiResetArmed = qfalse;
		// Read the configured devices in, rather than trusting whatever the
		// page was left showing. A device can have been unplugged since.
		UI_AudioSync();
		return;
	}
	if ( !Q_stricmp( cmd, "audioapply" ) ) {
		UI_AudioApply();
		return;
	}
	if ( !Q_stricmp( cmd, "home" ) ) {
		// The audio page is the only one reachable from inside a match, so it
		// is the only one whose BACK is not always the home screen.
		uiMenu           = ( uiMenu == MENU_AUDIO && uiAudioFromGame )
		                   ? MENU_INGAME : MENU_MAIN;
		uiResetArmed     = qfalse;
		// A row left waiting would otherwise still be armed on the way back in,
		// and the first key pressed on the home screen would be swallowed by it.
		uiCaptureAction  = -1;
		uiCaptureNote[0] = '\0';
		return;
	}
	/*
	Restoring defaults is internal only so that the note can be replaced in the
	same breath. `cf_binds reset` goes through the command buffer and lands next
	frame, so a note left over from a rebind would otherwise sit under a list
	that had just stopped agreeing with it.
	*/
	if ( !Q_stricmp( cmd, "bindsreset" ) ) {
		uiCaptureAction = -1;
		Q_strncpyz( uiCaptureNote, "every key back to its default",
		            sizeof( uiCaptureNote ) );
		trap_Cmd_ExecuteText( EXEC_APPEND, "cf_binds reset\n" );
		return;
	}
	/*
	UP and DOWN drive whichever page is open. One pair of buttons rather than a
	pair per screen: they are the same gesture, and the alternative is two more
	internal commands that differ only in which counter they touch.
	*/
	if ( !Q_stricmp( cmd, "down" ) ) {
		if ( uiMenu == MENU_CONTROLS ) {
			uiControlsScroll += CONTROLS_VISIBLE_LINES - 2;
		} else {
			uiStatsScroll += STATS_VISIBLE_LINES - 2;
		}
		return;
	}
	if ( !Q_stricmp( cmd, "up" ) ) {
		if ( uiMenu == MENU_CONTROLS ) {
			uiControlsScroll -= CONTROLS_VISIBLE_LINES - 2;
			if ( uiControlsScroll < 0 ) {
				uiControlsScroll = 0;
			}
		} else {
			uiStatsScroll -= STATS_VISIBLE_LINES - 2;
			if ( uiStatsScroll < 0 ) {
				uiStatsScroll = 0;
			}
		}
		return;
	}
	/*
	Two steps to wipe her, and this is the one place in the project where a
	confirmation is the right answer.

	The console's `voice_forget all` is deliberately blunt -- somebody who
	types it meant it. A button four inches from PLAY is a different thing: a
	misclick would take a relationship built over weeks, and there is no undo
	anywhere in the design because the whole store is the undo.
	*/
	if ( !Q_stricmp( cmd, "reset" ) ) {
		uiResetArmed = qtrue;
		return;
	}
	if ( !Q_stricmp( cmd, "resetgo" ) ) {
		uiResetArmed = qfalse;
		// Her memory, and the postgame card the home screen would otherwise
		// still be showing from a relationship that no longer exists.
		trap_Cmd_ExecuteText( EXEC_APPEND, "voice_forget all\n" );
		trap_Cmd_ExecuteText( EXEC_APPEND, UI_CLEAR_RESULT "\n" );
		uiStatsScroll = 0;
		return;
	}
}

/*
================
UI_WrapText

Break the helper's listing into rows that fit the page, on word boundaries.

Returns how many rows there are. Continuation rows begin with a space, which is
both the indent and the only marker the drawing needs to tell a wrapped line
from a new one.
================
*/
static int UI_WrapText( const char *text, char rows[][STATS_COLS + 1], int maxRows ) {
	int n = 0;

	while ( *text && n < maxRows ) {
		const char *end = strchr( text, '\n' );
		int         len = end ? (int)( end - text ) : (int)strlen( text );
		int         first = 1;

		// An empty source line is a blank row, and the spacing in the listing
		// is doing real work -- it separates the relationship from the facts
		// from the matches.
		if ( len == 0 ) {
			rows[n++][0] = '\0';
		}

		while ( len > 0 && n < maxRows ) {
			int width = first ? STATS_COLS : STATS_COLS - 4;
			int take  = len;
			int i;

			if ( take > width ) {
				take = width;
				// Back up to a space so words are not cut in half. If there is
				// no space at all, hard-break rather than loop forever.
				for ( i = take; i > 0; i-- ) {
					if ( text[i] == ' ' ) {
						take = i;
						break;
					}
				}
				if ( i == 0 ) {
					take = width;
				}
			}

			if ( first ) {
				memcpy( rows[n], text, take );
				rows[n][take] = '\0';
			} else {
				rows[n][0] = rows[n][1] = rows[n][2] = rows[n][3] = ' ';
				memcpy( rows[n] + 4, text, take );
				rows[n][take + 4] = '\0';
			}
			n++;

			text  += take;
			len   -= take;
			first  = 0;
			while ( len > 0 && *text == ' ' ) {
				text++;
				len--;
			}
		}

		if ( !end ) {
			break;
		}
		text = end + 1;
	}
	return n;
}

static void UI_DrawStats( void ) {
	static char text[MAX_STATS_TEXT];
	char        state[32];
	int         line, shown, total;
	float       y;

	UI_FillRect( 0, 0, 640, 480, colorBackground );
	UI_DrawStringCentred( 320, 24, "COMPANION", 12, 24, colorClaw );

	trap_Cvar_VariableStringBuffer( "cl_voiceState", state, sizeof( state ) );
	if ( !state[0] ) {
		Q_strncpyz( state, "off", sizeof( state ) );
	}
	UI_DrawStringCentred( 320, 52, va( "helper: %s", state ), 6, 12, colorTextDim );

	trap_VoiceMemory( text, sizeof( text ) );

	if ( !text[0] ) {
		/*
		No listing yet, and the reason matters to whoever is reading. A helper
		that is starting will fill this in on its own; one that is off or has
		failed never will, and a tester should not sit waiting for it.
		*/
		if ( !Q_stricmp( state, "ready" ) ) {
			UI_DrawStringCentred( 320, 200, "asking her...", 8, 16, colorTextDim );
		} else {
			UI_DrawStringCentred( 320, 190, "the companion helper is not running", 8, 16, colorError );
			UI_DrawStringCentred( 320, 214, "nothing here is being recorded", 6, 12, colorTextDim );
		}
	} else {
		static char rows[MAX_STATS_ROWS][STATS_COLS + 1];

		total = UI_WrapText( text, rows, MAX_STATS_ROWS );

		if ( uiStatsScroll > total - STATS_VISIBLE_LINES ) {
			uiStatsScroll = total - STATS_VISIBLE_LINES;
		}
		if ( uiStatsScroll < 0 ) {
			uiStatsScroll = 0;
		}

		y     = 76;
		shown = 0;
		for ( line = uiStatsScroll;
			  line < total && shown < STATS_VISIBLE_LINES;
			  line++, shown++ ) {
			// A leading space is a wrapped continuation; dimming it keeps a
			// two-line episode from reading as two entries.
			UI_DrawString( 24, y, rows[line], 6, 12,
						   ( rows[line][0] == ' ' ) ? colorTextDim : colorText );
			y += 14;
		}

		if ( total > STATS_VISIBLE_LINES ) {
			UI_DrawStringCentred( 320, 376, va( "line %i of %i",
												uiStatsScroll + shown, total ),
								  6, 12, colorTextDim );
		}
	}

	UI_BeginButtons();
	UI_AddButton( 24, 396, 110, 32, "BACK", "!home" );
	UI_AddButton( 142, 396, 90, 32, "UP", "!up" );
	UI_AddButton( 240, 396, 90, 32, "DOWN", "!down" );

	if ( uiResetArmed ) {
		UI_AddButton( 400, 396, 216, 32, "CONFIRM - WIPE HER", "!resetgo" );
		UI_DrawStringCentred( 508, 436,
							  "this cannot be undone", 6, 12, colorError );
	} else {
		UI_AddButton( 400, 396, 216, 32, "RESET COMPANION", "!reset" );
	}
	UI_DrawButtons();
}

/*
===========================================================================

THE CONTROLS SCREEN

Every key the game listens to, on one page, read from the SAME table the binds
themselves come from (code/cf_shared/cf_binds.c). Nothing here is a written-down
list of what the keys are supposed to be -- it asks the engine what each action
is actually bound to right now, so a page that disagrees with the game is not a
state this can get into.

That matters more here than it looks. The whole reason cf_binds.c exists is that
binds have already been silently lost once, to a stale config's `unbindall`, and
the symptom was a key that did nothing with no error anywhere. A player who can
SEE that "Follow me" is unbound can say so; one who cannot just thinks the game
is broken.

An action with no key shows as "--" rather than being hidden, for the same
reason: the migration deliberately refuses to steal a default key that is
already in use, and when it does that the only evidence is this page.

It is read-only for now. Rebinding already exists underneath it --
CF_BindActionSetKey, and the `cf_bind` console command -- so adding click-to-set
is a key-capture mode on top of this, not new machinery.

===========================================================================
*/

/*
================
UI_ControlsRowCount

Rows, counting the group headings and the blank line before each. The list is
built the same way twice -- once to measure, once to draw -- rather than cached,
because it is twenty-six rows once a frame and a cache that went stale would be
showing the wrong keys, which is the one thing this page must not do.
================
*/
static int UI_ControlsRows( int firstRow, int maxRows, float *yOut ) {
	const char *lastGroup = NULL;
	int         i, row = 0, drawn = 0;
	float       y = yOut ? *yOut : 0;

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		const cf_bindAction_t *a = CF_BindAction( i );
		int                    key;
		char                   keyName[32];

		if ( !a ) {
			continue;
		}

		// A heading, and a blank line above it once past the first group.
		if ( !lastGroup || Q_stricmp( a->group, lastGroup ) ) {
			if ( lastGroup ) {
				if ( yOut && row >= firstRow && drawn < maxRows ) {
					y += 14;
					drawn++;
				}
				row++;
			}
			if ( yOut && row >= firstRow && drawn < maxRows ) {
				UI_DrawString( 40, y, a->group, 7, 14, colorClaw );
				y += 16;
				drawn++;
			}
			row++;
			lastGroup = a->group;
		}

		if ( yOut && row >= firstRow && drawn < maxRows ) {
			qboolean capturing = (qboolean)( uiCaptureAction == i );
			qboolean hot;

			// The row under the cursor, so a clickable list looks clickable.
			hot = (qboolean)( uis.cursorx >= 48 && uis.cursorx < 600
			                  && uis.cursory >= y - 2 && uis.cursory < y + 12 );

			if ( capturing || hot ) {
				UI_FillRect( 48, y - 2, 552, 14,
				             capturing ? colorButtonHot : colorButton );
			}

			key = CF_BindActionKey( a );
			if ( key >= 0 ) {
				trap_Key_KeynumToStringBuf( key, keyName, sizeof( keyName ) );
			} else {
				Q_strncpyz( keyName, "--", sizeof( keyName ) );
			}

			UI_DrawString( 56, y, a->display, 6, 12,
			               capturing ? colorClaw : colorText );

			if ( capturing ) {
				UI_DrawString( 380, y, "PRESS A KEY", 6, 12, colorClaw );
			} else {
				UI_DrawString( 380, y, keyName, 6, 12,
				               key >= 0 ? colorText : colorError );
			}

			// Remembered so a click can be tested against it. Only real action
			// rows go in -- a heading is not a thing you can bind.
			if ( uiNumControlRows < CONTROLS_VISIBLE_LINES ) {
				uiControlRows[uiNumControlRows].y      = y;
				uiControlRows[uiNumControlRows].action = i;
				uiNumControlRows++;
			}

			y += 14;
			drawn++;
		}
		row++;
	}

	if ( yOut ) {
		*yOut = y;
	}
	return row;
}

/*
================
UI_ActionOnKey

Which action is currently bound to this key, or NULL.

Looked up by comparing the engine's binding string against the table's commands
rather than by remembering who we gave it to, for the same reason the page reads
live bindings: the key may have been set from the console, or by default.cfg, or
by a migration, and none of those told this file anything.
================
*/
static const cf_bindAction_t *UI_ActionOnKey( int key ) {
	char binding[MAX_STRING_CHARS];
	int  i;

	if ( key < 0 ) {
		return NULL;
	}

	trap_Key_GetBindingBuf( key, binding, sizeof( binding ) );
	if ( !binding[0] ) {
		return NULL;
	}

	for ( i = 0; i < CF_BindActionCount(); i++ ) {
		const cf_bindAction_t *a = CF_BindAction( i );

		if ( a && !Q_stricmp( a->command, binding ) ) {
			return a;
		}
	}

	return NULL;
}

/*
================
UI_CaptureKey

The next key pressed after clicking a row.

EVERY key is a candidate, which is why this runs before UI_KeyEvent's switch
rather than inside it: ENTER, TAB and the arrows are all ordinary keys somebody
may want to bind, and they are also the menu's own navigation. There is no way
to have both except by suspending navigation completely while a row is waiting.

Escape is the one exception and it cancels, because a screen that can only be
left by binding something is a trap -- and escape is what everybody presses.
Backspace clears instead of binding, which is the only route to "I want this on
no key at all".
================
*/
static void UI_CaptureKey( int key ) {
	const cf_bindAction_t *action = CF_BindAction( uiCaptureAction );
	const cf_bindAction_t *previous;

	uiCaptureAction = -1;

	if ( !action ) {
		return;
	}

	if ( key == K_ESCAPE ) {
		Q_strncpyz( uiCaptureNote, "cancelled", sizeof( uiCaptureNote ) );
		return;
	}

	if ( key == K_BACKSPACE || key == K_DEL ) {
		CF_BindActionClear( action );
		Com_sprintf( uiCaptureNote, sizeof( uiCaptureNote ),
		             "%s is now unbound", action->display );
		return;
	}

	/*
	Asked BEFORE the rebind, because afterwards the key belongs to us and
	whatever it displaced is simply gone -- there would be nothing left to
	name. Comparing against the action itself so that rebinding something to
	the key it already has does not report it stealing from itself.
	*/
	previous = UI_ActionOnKey( key );

	CF_BindActionSetKey( action, key );

	{
		char keyName[32];

		trap_Key_KeynumToStringBuf( key, keyName, sizeof( keyName ) );

		if ( previous && previous != action ) {
			Com_sprintf( uiCaptureNote, sizeof( uiCaptureNote ),
			             "%s is on %s  --  took it from %s, now unbound",
			             action->display, keyName, previous->display );
		} else {
			Com_sprintf( uiCaptureNote, sizeof( uiCaptureNote ),
			             "%s is on %s", action->display, keyName );
		}
	}
}

/*
================
UI_ControlsClick

A click on the list. Returns true if it landed on a row, so the caller knows
not to also treat it as a button press.
================
*/
static qboolean UI_ControlsClick( void ) {
	int i;

	for ( i = 0; i < uiNumControlRows; i++ ) {
		if ( uis.cursorx >= 48 && uis.cursorx < 600
		     && uis.cursory >= uiControlRows[i].y - 2
		     && uis.cursory < uiControlRows[i].y + 12 ) {
			uiCaptureAction  = uiControlRows[i].action;
			uiCaptureNote[0] = '\0';
			return qtrue;
		}
	}

	return qfalse;
}

/*
===========================================================================
The audio screen.

Six rows -- three volumes, a toggle and two devices -- and the halves behave
differently because the cvars underneath them do.

VOLUMES AND THE TOGGLE APPLY AS YOU CLICK. cl_voiceVolume, s_volume,
s_musicvolume and cg_subtitles are plain archived cvars, so the only honest way
to set one is to hear (or read) it change.

COMPANION IS FIRST because it is the one most players will come here for. It
rides under s_volume rather than beside it -- see cl_voiceVolume in cl_voice.c
-- so it is the balance between her and the fight, not an absolute level.

SUBTITLES sits with the volumes rather than under graphics because somebody who
cannot make out what she said reaches for the audio page, and cg_subtitles is
the answer to that. The UI registers it as well as cgame does: see UI_Init.

DEVICES DO NOT, and cannot. s_device and s_captureDevice are CVAR_LATCH -- the
device is opened once during sound init -- so a write is pending until an
snd_restart. Worse for a menu, a latched cvar keeps REPORTING its old value
until then, so a screen that set the cvar and read it back to draw itself would
show the previous device and look broken.

So the selection lives here as an index and is only written on APPLY. That is
also the better behaviour: cycling through six devices would otherwise restart
the sound system six times.

WHY DEVICE PICKERS EXIST AT ALL is written at length in sdl_snd.c: SDL's idea
of the default input does not always agree with Windows', and when it picks a
virtual device the open SUCCEEDS and every sample is silence. There is no error
to notice. That cost an evening with a microphone that worked everywhere else,
and the same trap exists on the playback side with a headset and an HDMI
monitor.
===========================================================================
*/
// Two more rows than there used to be, so they start higher and sit closer
// together. A volume row is 30px tall (the label, then the percentage under the
// bar), which is what sets the floor on the step.
#define AUDIO_ROW_Y     96
#define AUDIO_ROW_STEP  46
#define AUDIO_MINUS_X   348
#define AUDIO_PLUS_X    556
#define AUDIO_BAR_X     388
#define AUDIO_BAR_W     156
#define AUDIO_HIT_H     22
// What fits between the device column at x=348 and the right margin, at 6px
// per character.
#define AUDIO_NAME_CHARS 46

/*
The rows, in the order they are drawn and clicked.

One table rather than two switch statements, because the previous shape --
"rows 0 and 1 are volumes, the rest are devices" -- was written in two places
and the draw and the hit test had to be kept agreeing by hand.
*/
typedef enum {
	AUDIOROW_VOLUME,   // minus / bar / plus, writes cvar directly
	AUDIOROW_TOGGLE,   // click anywhere on the row to flip cvar
	AUDIOROW_DEVICE    // click anywhere on the row to cycle, APPLY to commit
} audioRowKind_t;

typedef struct {
	audioRowKind_t kind;
	const char     *label;
	const char     *cvar;     // VOLUME and TOGGLE
	int            capture;   // DEVICE: 0 playback, 1 microphone
} audioRow_t;

static const audioRow_t uiAudioRows[] = {
	{ AUDIOROW_VOLUME, "COMPANION",  "cl_voiceVolume", 0 },
	{ AUDIOROW_VOLUME, "EFFECTS",    "s_volume",       0 },
	{ AUDIOROW_VOLUME, "MUSIC",      "s_musicvolume",  0 },
	{ AUDIOROW_TOGGLE, "SUBTITLES",  "cg_subtitles",   0 },
	{ AUDIOROW_DEVICE, "OUTPUT",     NULL,             0 },
	{ AUDIOROW_DEVICE, "MICROPHONE", NULL,             1 }
};

// Cast, because ARRAY_LEN is a size_t and every loop over the rows counts with
// an int -- an unsigned comparison here is a warning at best.
#define AUDIO_NUM_ROWS ( (int)ARRAY_LEN( uiAudioRows ) )

// -1 is "whatever the system calls the default", which is what an empty cvar
// means and is the right answer for almost everybody.
static int uiAudioOut = -1;
static int uiAudioIn  = -1;
static char uiAudioNote[80];

static int UI_AudioFindDevice( int capture, const char *want ) {
	int i, count;

	if ( !want[0] ) {
		return -1;
	}
	count = trap_AudioDevices( capture, -1, NULL, 0 );
	for ( i = 0; i < count; i++ ) {
		char name[MAX_QPATH];

		trap_AudioDevices( capture, i, name, sizeof( name ) );
		if ( !Q_stricmp( name, want ) ) {
			return i;
		}
	}
	// Named a device that is no longer present -- unplugged since it was
	// chosen. The default is the only safe thing to show.
	return -1;
}

// Called when the page opens, so the pending selection starts from what is
// actually configured rather than from whatever was left behind last time.
static void UI_AudioSync( void ) {
	char cur[MAX_QPATH];

	trap_Cvar_VariableStringBuffer( "s_device", cur, sizeof( cur ) );
	uiAudioOut = UI_AudioFindDevice( 0, cur );

	trap_Cvar_VariableStringBuffer( "s_captureDevice", cur, sizeof( cur ) );
	uiAudioIn = UI_AudioFindDevice( 1, cur );

	uiAudioNote[0] = '\0';
}

static void UI_AudioDeviceLabel( int capture, int index, char *out, int outSize ) {
	if ( index < 0 ) {
		Q_strncpyz( out, "System default", outSize );
		return;
	}
	trap_AudioDevices( capture, index, out, outSize );
	if ( !out[0] ) {
		Q_strncpyz( out, "System default", outSize );
	}
}

// Walks default -> 0 -> 1 -> ... -> default. The default is in the cycle rather
// than being a separate control because it is a legitimate choice, and for most
// players the correct one.
static void UI_AudioCycle( int capture ) {
	int *sel = capture ? &uiAudioIn : &uiAudioOut;
	int count = trap_AudioDevices( capture, -1, NULL, 0 );

	if ( count <= 0 ) {
		Q_strncpyz( uiAudioNote, "no devices of that kind were found",
		            sizeof( uiAudioNote ) );
		return;
	}
	*sel = ( *sel + 1 >= count ) ? -1 : *sel + 1;
	Q_strncpyz( uiAudioNote, "APPLY to switch device", sizeof( uiAudioNote ) );
}

static void UI_AudioVolume( const char *cvar, float delta ) {
	float v = trap_Cvar_VariableValue( cvar ) + delta;

	if ( v < 0.0f ) {
		v = 0.0f;
	} else if ( v > 1.0f ) {
		v = 1.0f;
	}
	trap_Cvar_Set( cvar, va( "%.2f", v ) );
}

// Anything non-zero reads as on, so a cvar somebody set to 2 from the console
// turns off on the first click rather than on the second.
static void UI_AudioToggle( const char *cvar ) {
	trap_Cvar_Set( cvar, trap_Cvar_VariableValue( cvar ) ? "0" : "1" );
}

static void UI_AudioApply( void ) {
	char name[MAX_QPATH];

	if ( uiAudioOut < 0 ) {
		trap_Cvar_Set( "s_device", "" );
	} else {
		trap_AudioDevices( 0, uiAudioOut, name, sizeof( name ) );
		trap_Cvar_Set( "s_device", name );
	}

	if ( uiAudioIn < 0 ) {
		trap_Cvar_Set( "s_captureDevice", "" );
	} else {
		trap_AudioDevices( 1, uiAudioIn, name, sizeof( name ) );
		trap_Cvar_Set( "s_captureDevice", name );
	}

	// The only thing that makes a latched device cvar take effect.
	trap_Cmd_ExecuteText( EXEC_APPEND, "snd_restart\n" );
	Q_strncpyz( uiAudioNote, "sound restarted", sizeof( uiAudioNote ) );
}

/*
================
UI_AudioClick

Returns true if the click landed on a row, so the caller does not also treat it
as a button press.
================
*/
static qboolean UI_AudioClick( void ) {
	float y = AUDIO_ROW_Y;
	int   i;

	for ( i = 0; i < AUDIO_NUM_ROWS; i++, y += AUDIO_ROW_STEP ) {
		const audioRow_t *row = &uiAudioRows[i];

		if ( uis.cursory < y - 4 || uis.cursory >= y + AUDIO_HIT_H ) {
			continue;
		}

		if ( row->kind == AUDIOROW_VOLUME ) {
			if ( uis.cursorx >= AUDIO_MINUS_X - 6 && uis.cursorx < AUDIO_MINUS_X + 26 ) {
				UI_AudioVolume( row->cvar, -0.05f );
				return qtrue;
			}
			if ( uis.cursorx >= AUDIO_PLUS_X - 6 && uis.cursorx < AUDIO_PLUS_X + 26 ) {
				UI_AudioVolume( row->cvar, 0.05f );
				return qtrue;
			}
			continue;
		}

		if ( uis.cursorx >= AUDIO_MINUS_X - 6 && uis.cursorx < 604 ) {
			if ( row->kind == AUDIOROW_TOGGLE ) {
				UI_AudioToggle( row->cvar );
			} else {
				UI_AudioCycle( row->capture );
			}
			return qtrue;
		}
	}

	return qfalse;
}

static void UI_DrawAudioRow( float y, const char *label, const char *value,
                             qboolean isVolume, float frac ) {
	UI_DrawString( 60, y, label, 8, 16, colorText );

	if ( isVolume ) {
		UI_DrawString( AUDIO_MINUS_X, y, "-", 8, 16, colorClaw );
		UI_DrawString( AUDIO_PLUS_X, y, "+", 8, 16, colorClaw );

		// The buttons clamp, but the console does not, and a cvar set to 4
		// from there would otherwise draw a bar out past the right margin.
		if ( frac > 1.0f ) {
			frac = 1.0f;
		}

		// A bar as well as a number: the number is what you set, the bar is
		// what it feels like, and at a glance the bar is the one that reads.
		UI_FillRect( AUDIO_BAR_X, y + 5, AUDIO_BAR_W, 6, colorTextDim );
		if ( frac > 0.0f ) {
			UI_FillRect( AUDIO_BAR_X, y + 5, AUDIO_BAR_W * frac, 6, colorClaw );
		}
		UI_DrawStringCentred( AUDIO_BAR_X + AUDIO_BAR_W / 2, y + 18, value, 6, 12,
		                      colorTextDim );
		return;
	}

	/*
	TRUNCATED, and at 6px rather than 7.

	Device names are written by hardware vendors and are not short: the first
	microphone this was tested against reported "Microphone (High Definition
	Audio Device)", 41 characters, which ran off the right edge of the screen.
	A name that is cut off at the window edge looks like a rendering bug; one
	cut off with an ellipsis looks like a long name, which is what it is.
	*/
	{
		char fit[AUDIO_NAME_CHARS + 1];
		int  len = (int)strlen( value );

		if ( len <= AUDIO_NAME_CHARS ) {
			UI_DrawString( AUDIO_MINUS_X, y, value, 6, 12, colorClaw );
			return;
		}
		Q_strncpyz( fit, value, sizeof( fit ) );
		fit[AUDIO_NAME_CHARS - 3] = '.';
		fit[AUDIO_NAME_CHARS - 2] = '.';
		fit[AUDIO_NAME_CHARS - 1] = '.';
		fit[AUDIO_NAME_CHARS] = '\0';
		UI_DrawString( AUDIO_MINUS_X, y, fit, 6, 12, colorClaw );
	}
}

static void UI_DrawAudio( void ) {
	float y = AUDIO_ROW_Y;
	int   i;

	UI_FillRect( 0, 0, 640, 480, colorBackground );
	UI_DrawStringCentred( 320, 24, "AUDIO", 12, 24, colorClaw );

	for ( i = 0; i < AUDIO_NUM_ROWS; i++, y += AUDIO_ROW_STEP ) {
		const audioRow_t *row = &uiAudioRows[i];
		char             name[MAX_QPATH];
		float            v;

		switch ( row->kind ) {
		case AUDIOROW_VOLUME:
			v = trap_Cvar_VariableValue( row->cvar );
			UI_DrawAudioRow( y, row->label,
			                 va( "%i%%", (int)( v * 100.0f + 0.5f ) ), qtrue, v );
			break;

		case AUDIOROW_TOGGLE:
			UI_DrawAudioRow( y, row->label,
			                 trap_Cvar_VariableValue( row->cvar ) ? "ON" : "OFF",
			                 qfalse, 0.0f );
			break;

		case AUDIOROW_DEVICE:
			UI_AudioDeviceLabel( row->capture,
			                     row->capture ? uiAudioIn : uiAudioOut,
			                     name, sizeof( name ) );
			UI_DrawAudioRow( y, row->label, name, qfalse, 0.0f );
			break;
		}
	}

	UI_DrawStringCentred( 320, 368, "click the subtitle or device rows to change them",
	                      6, 12, colorTextDim );

	UI_BeginButtons();
	UI_AddButton( 24, 396, 110, 32, "BACK", "!home" );
	UI_AddButton( 440, 396, 176, 32, "APPLY DEVICES", "!audioapply" );
	UI_DrawButtons();

	if ( uiAudioNote[0] ) {
		UI_DrawStringCentred( 320, 440, uiAudioNote, 7, 14, colorClaw );
	}
}

static void UI_DrawControls( void ) {
	int   total;
	float y;

	UI_FillRect( 0, 0, 640, 480, colorBackground );
	UI_DrawStringCentred( 320, 24, "CONTROLS", 12, 24, colorClaw );

	uiNumControlRows = 0;

	total = UI_ControlsRows( 0, 0, NULL );

	if ( uiControlsScroll > total - CONTROLS_VISIBLE_LINES ) {
		uiControlsScroll = total - CONTROLS_VISIBLE_LINES;
	}
	if ( uiControlsScroll < 0 ) {
		uiControlsScroll = 0;
	}

	y = 60;
	UI_ControlsRows( uiControlsScroll, CONTROLS_VISIBLE_LINES, &y );

	if ( total > CONTROLS_VISIBLE_LINES ) {
		UI_DrawStringCentred( 320, 376, va( "%i of %i",
		                                    uiControlsScroll + CONTROLS_VISIBLE_LINES,
		                                    total ), 6, 12, colorTextDim );
	}

	UI_BeginButtons();
	UI_AddButton( 24, 396, 110, 32, "BACK", "!home" );
	UI_AddButton( 142, 396, 90, 32, "UP", "!up" );
	UI_AddButton( 240, 396, 90, 32, "DOWN", "!down" );
	UI_AddButton( 440, 396, 176, 32, "RESET TO DEFAULTS", "!bindsreset" );
	UI_DrawButtons();

	if ( uiCaptureAction >= 0 ) {
		const cf_bindAction_t *a = CF_BindAction( uiCaptureAction );

		UI_DrawStringCentred( 320, 440,
		                      va( "press a key for %s", a ? a->display : "it" ),
		                      7, 14, colorClaw );
		UI_DrawStringCentred( 320, 458,
		                      "ESC cancels     BACKSPACE unbinds",
		                      6, 12, colorTextDim );
	} else if ( uiCaptureNote[0] ) {
		UI_DrawStringCentred( 320, 440, uiCaptureNote, 6, 12, colorClaw );
		UI_DrawStringCentred( 320, 458, "click an action to change its key",
		                      6, 12, colorTextDim );
	} else {
		/*
		Alt+Enter is not in the table and cannot be, because it is a CHORD and
		the engine's bind system holds one key per binding. It is handled
		directly in CL_KeyDownEvent, before the UI key catcher gets a look --
		which is also why it is the only one of these that works while this menu
		is open. Saying so here is the only place a player would find that out.
		*/
		UI_DrawStringCentred( 320, 440, "click an action to change its key",
		                      6, 12, colorTextDim );
		UI_DrawStringCentred( 320, 458, "ALT+ENTER  fullscreen, and it works anywhere",
		                      6, 12, colorTextDim );
	}
}

static void UI_DrawInGame( void ) {
	UI_FillRect( 0, 0, 640, 480, colorOverlay );
	UI_DrawClawMark( 320, 140, 1.0f, colorClaw );
	UI_DrawStringCentred( 320, 190, "PAUSED", 14, 28, colorClaw );

	UI_BeginButtons();
	UI_AddButton( 200, 250, 240, 40, "RESUME", "togglemenu" );
	/*
	AUDIO here as well as on the home screen, and it is the more important of
	the two placements. The moment a player discovers their volume is wrong, or
	that the sound is coming out of the wrong device, is the moment they are in
	a match -- and the only route to the home screen from here is LEAVE MATCH.
	*/
	UI_AddButton( 200, 298, 240, 40, "AUDIO", "!audio" );
	UI_AddButton( 200, 346, 240, 40, "LEAVE MATCH", "disconnect" );
	UI_DrawButtons();

	UI_DrawStringCentred( 320, 410, "escape to resume", 6, 12, colorTextDim );
}

static void UI_Refresh( int realtime ) {
	(void)realtime;

	if ( uiMenu == MENU_MAIN ) {
		char state[32], status[256];

		// before the draw, so a queued PLAY AGAIN starts its search on the same
		// frame the menu comes up rather than flashing the home screen first
		UI_MatchmakingState( state, sizeof( state ), status, sizeof( status ) );
		UI_CheckPlayAgain( state );

		UI_DrawHome();
	} else if ( uiMenu == MENU_STATS ) {
		UI_DrawStats();
	} else if ( uiMenu == MENU_CONTROLS ) {
		UI_DrawControls();
	} else if ( uiMenu == MENU_AUDIO ) {
		UI_DrawAudio();
	} else if ( uiMenu == MENU_INGAME ) {
		UI_DrawInGame();
	} else {
		return;
	}

	// last, so nothing can draw over it
	UI_DrawCursor();
}

/*
================
UI_DrawConnectScreen
================
*/
static void UI_DrawConnectScreen( qboolean overlay ) {
	uiClientState_t cs;
	float           phase;

	trap_GetClientState( &cs );

	if ( !overlay ) {
		UI_FillRect( 0, 0, 640, 480, colorBackground );
		UI_DrawClawMark( 320, 190, 1.2f, colorClawDim );
		UI_DrawStringCentred( 320, 260, "CONNECTING", 12, 24, colorText );
		if ( cs.servername[0] ) {
			UI_DrawStringCentred( 320, 292, cs.servername, 8, 16, colorTextDim );
		}
		if ( cs.messageString[0] ) {
			UI_DrawStringCentred( 320, 380, cs.messageString, 8, 16, colorClaw );
		}
	}

	phase = ( trap_Milliseconds() % 1400 ) / 1400.0f;
	UI_FillRect( 220 + 200 * phase, 330, 20, 3, colorClaw );
}

/*
=================
UI_Init
=================
*/
static void UI_Init( qboolean inGameLoad ) {
	memset( &uis, 0, sizeof( uis ) );

	trap_GetGlconfig( &uis.glconfig );

	uis.xscale = uis.glconfig.vidWidth / 640.0f;
	uis.yscale = uis.glconfig.vidHeight / 480.0f;

	uis.white = trap_R_RegisterShaderNoMip( "ui/white" );
	uis.charset = trap_R_RegisterShaderNoMip( "gfx/2d/bigchars" );
	uis.startTime = trap_Milliseconds();

	UI_CentreCursor();

	/*
	Bring the player's key bindings up to date with this build's defaults.

	Here because this runs AFTER both default.cfg and the saved config have been
	exec'd, and the saved config is precisely what erases defaults it has never
	heard of. Anywhere earlier and it would be undone by the thing it exists to
	work around. See ui_binds.c.
	*/
	CF_BindsInit();

	/*
	The subtitle row on the audio page writes cg_subtitles, which cgame owns
	and only registers once a match has been loaded. Register it here too.

	Without this, turning subtitles off from the main menu of a fresh launch
	writes a cvar the engine has never heard of, which is created
	CVAR_USER_CREATED and therefore unarchived -- the row would read OFF for
	the rest of the session and be back ON at the next launch. Registering the
	same cvar from two modules is ordinary; the flags are OR'd together and the
	value already set is kept.
	*/
	trap_Cvar_Register( NULL, "cg_subtitles", "1", CVAR_ARCHIVE );

	if ( !inGameLoad ) {
		// The console used to be opened here, because it was the only way in.
		// The menu is, now.
		UI_Printf( "\n" );
		UI_Printf( "^3catfight^7 -- PLAY queues for a match.\n" );
		UI_Printf( "  cg_showSpeed 1     speed bar, for tuning movement\n" );
		UI_Printf( "  cg_thirdPerson 1   (needs sv_cheats 1)\n" );
		UI_Printf( "\n" );
	}
}

static void UI_Shutdown( void ) {
}

/*
=================
UI_ConsoleCommand
=================
*/
static qboolean UI_ConsoleCommand( int realTime ) {
	char cmd[MAX_TOKEN_CHARS];

	(void)realTime;

	trap_Argv( 0, cmd, sizeof( cmd ) );

	/*
	Anything not claimed here falls through to being forwarded to the server,
	so returning qtrue for a command we do not own would swallow it silently --
	which is a whole class of "the key does nothing" bug and is exactly the sort
	of thing that took an hour to find once already.
	*/
	if ( CF_BindsConsoleCommand( cmd ) ) {
		return qtrue;
	}

	/*
	The stats page, from the console.

	Worth having beyond the button. A playtester who wants to glance at her
	state between matches can bind a key to this, and it is the only route in
	that survives the home screen being somewhere else -- the button lives on
	a screen you cannot see while a match is loading, which is exactly when
	somebody wants to check whether she is about to be strange.
	*/
	if ( !Q_stricmp( cmd, "ui_stats" ) ) {
		UI_Internal( "stats" );
		return qtrue;
	}

	return qfalse;
}

/*
================
vmMain
================
*/
Q_EXPORT intptr_t vmMain( int command, int arg0, int arg1, int arg2, int arg3, int arg4,
                          int arg5, int arg6, int arg7, int arg8, int arg9,
                          int arg10, int arg11 ) {
	switch ( command ) {
	case UI_GETAPIVERSION:
		return UI_API_VERSION;

	case UI_INIT:
		UI_Init( arg0 );
		return 0;

	case UI_SHUTDOWN:
		UI_Shutdown();
		return 0;

	case UI_KEY_EVENT:
		UI_KeyEvent( arg0, arg1 );
		return 0;

	case UI_MOUSE_EVENT:
		UI_MouseEvent( arg0, arg1 );
		return 0;

	case UI_REFRESH:
		UI_Refresh( arg0 );
		return 0;

	case UI_IS_FULLSCREEN:
		// only the main menu covers the world; the in-game menu is an overlay
		return uiMenu == MENU_MAIN;

	case UI_SET_ACTIVE_MENU:
		UI_SetActiveMenu( arg0 );
		return 0;

	case UI_CONSOLE_COMMAND:
		return UI_ConsoleCommand( arg0 );

	case UI_DRAW_CONNECT_SCREEN:
		UI_DrawConnectScreen( arg0 );
		return 0;

	case UI_HASUNIQUECDKEY:
		return qtrue;
	}

	return -1;
}
