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

uiStatic_t uis;

typedef enum {
	MENU_NONE,
	MENU_MAIN,     // not connected -- we own the whole screen
	MENU_INGAME    // connected, escape pressed -- we overlay the world
} cfMenu_t;

static cfMenu_t uiMenu;

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
#define MAX_MENU_BUTTONS 6

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

static void UI_ActivateButton( int index ) {
	if ( index < 0 || index >= uiNumButtons ) {
		return;
	}
	if ( !uiButtons[index].cmd[0] ) {
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

	switch ( key ) {
	case K_ESCAPE:
		if ( uiMenu == MENU_INGAME ) {
			UI_SetActiveMenu( UIMENU_NONE );
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
			y = 298;
			UI_AddButton( 200, y, 240, 38, "PLAY AGAIN", "mm_find" );
			y += 42;
			// DISMISS, not BACK: there is nothing behind this screen. It only
			// retires the card, leaving the ordinary home screen.
			UI_AddButton( 200, y, 240, 38, "DISMISS", UI_CLEAR_RESULT );
			y += 42;
			UI_AddButton( 200, y, 240, 38, "QUIT", "quit" );
		} else {
			y = 274;
			UI_AddButton( 200, y, 240, 40, "PLAY", "mm_find" );
			y += 48;
			// There used to be a PRACTICE button here, running `map cf_test`.
			// It is gone on purpose: `map` starts a listen server, which needs
			// the game module in the shipped package, and shipping that module
			// is the one thing the licensing decision says we do not do. Every
			// route into the game now goes through the pool.
			UI_AddButton( 200, y, 240, 40, "QUIT", "quit" );
		}
	}

	UI_DrawButtons();

	trap_Cvar_VariableStringBuffer( "mm_server", server, sizeof( server ) );
	if ( server[0] ) {
		UI_DrawStringCentred( 320, 436, va( "matchmaker  %s", server ), 6, 12, colorTextDim );
	}
	UI_DrawStringCentred( 320, 454, "~ for the console", 6, 12, colorTextDim );
}

static void UI_DrawInGame( void ) {
	UI_FillRect( 0, 0, 640, 480, colorOverlay );
	UI_DrawClawMark( 320, 140, 1.0f, colorClaw );
	UI_DrawStringCentred( 320, 190, "PAUSED", 14, 28, colorClaw );

	UI_BeginButtons();
	UI_AddButton( 200, 250, 240, 40, "RESUME", "togglemenu" );
	UI_AddButton( 200, 298, 240, 40, "LEAVE MATCH", "disconnect" );
	UI_DrawButtons();

	UI_DrawStringCentred( 320, 366, "escape to resume", 6, 12, colorTextDim );
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
	(void)realTime;
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
