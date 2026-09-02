/*
===========================================================================
catfight -- user interface module, internal header.
===========================================================================
*/

#ifndef CF_UI_LOCAL_H
#define CF_UI_LOCAL_H

#include "../cf_shared/cf_shared.h"
#include "../renderercommon/tr_types.h"
#include "../ui/ui_public.h"
#include "../client/keycodes.h"

typedef struct {
	qboolean   active;          // a menu is up
	glconfig_t glconfig;
	float      xscale, yscale;
	qhandle_t  white;
	qhandle_t  charset;
	int        startTime;

	// The engine hides the OS cursor and hands us relative motion while a menu
	// holds the key catcher, so the cursor is ours to track and ours to draw.
	// Kept in the same virtual 640x480 space everything else is laid out in.
	float      cursorx, cursory;
} uiStatic_t;

extern uiStatic_t uis;

//
// ui_syscalls.c
//
void        trap_Print( const char *string );
void        trap_Error( const char *string ) Q_NO_RETURN;
int         trap_Milliseconds( void );
void        trap_Cvar_Set( const char *var_name, const char *value );
float       trap_Cvar_VariableValue( const char *var_name );
void        trap_Cvar_VariableStringBuffer( const char *var_name, char *buffer, int bufsize );
void        trap_Cvar_Register( vmCvar_t *cvar, const char *var_name, const char *value, int flags );
void        trap_Cvar_Update( vmCvar_t *cvar );
int         trap_Argc( void );
void        trap_Argv( int n, char *buffer, int bufferLength );
void        trap_Cmd_ExecuteText( int exec_when, const char *text );
qhandle_t   trap_R_RegisterShaderNoMip( const char *name );
void        trap_R_SetColor( const float *rgba );
void        trap_R_DrawStretchPic( float x, float y, float w, float h,
                                   float s1, float t1, float s2, float t2, qhandle_t hShader );
void        trap_UpdateScreen( void );
void        trap_GetGlconfig( glconfig_t *glconfig );
void        trap_GetClientState( uiClientState_t *state );
void        trap_GetConfigString( int index, char *buff, int buffsize );
int         trap_Key_GetCatcher( void );
void        trap_Key_SetCatcher( int catcher );
void        trap_Key_ClearStates( void );
int         trap_Key_IsDown( int keynum );

#endif // CF_UI_LOCAL_H
