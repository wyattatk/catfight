/*
===========================================================================
catfight -- user interface syscall glue.
===========================================================================
*/

#include "ui_local.h"

static intptr_t ( QDECL *syscall )( intptr_t arg, ... ) = ( intptr_t ( QDECL * )( intptr_t, ... ) ) - 1;

Q_EXPORT void dllEntry( intptr_t ( QDECL *syscallptr )( intptr_t arg, ... ) ) {
	syscall = syscallptr;
}

static int PASSFLOAT( float x ) {
	floatint_t fi;
	fi.f = x;
	return fi.i;
}

void trap_Print( const char *string ) {
	syscall( UI_PRINT, string );
}

void trap_Error( const char *string ) {
	syscall( UI_ERROR, string );
	exit( 1 );
}

int trap_Milliseconds( void ) {
	return syscall( UI_MILLISECONDS );
}

void trap_Cvar_Set( const char *var_name, const char *value ) {
	syscall( UI_CVAR_SET, var_name, value );
}

float trap_Cvar_VariableValue( const char *var_name ) {
	floatint_t fi;
	fi.i = syscall( UI_CVAR_VARIABLEVALUE, var_name );
	return fi.f;
}

void trap_Cvar_VariableStringBuffer( const char *var_name, char *buffer, int bufsize ) {
	syscall( UI_CVAR_VARIABLESTRINGBUFFER, var_name, buffer, bufsize );
}

void trap_Cvar_Register( vmCvar_t *cvar, const char *var_name, const char *value, int flags ) {
	syscall( UI_CVAR_REGISTER, cvar, var_name, value, flags );
}

void trap_Cvar_Update( vmCvar_t *cvar ) {
	syscall( UI_CVAR_UPDATE, cvar );
}

int trap_Argc( void ) {
	return syscall( UI_ARGC );
}

void trap_Argv( int n, char *buffer, int bufferLength ) {
	syscall( UI_ARGV, n, buffer, bufferLength );
}

void trap_Cmd_ExecuteText( int exec_when, const char *text ) {
	syscall( UI_CMD_EXECUTETEXT, exec_when, text );
}

qhandle_t trap_R_RegisterShaderNoMip( const char *name ) {
	return syscall( UI_R_REGISTERSHADERNOMIP, name );
}

void trap_R_SetColor( const float *rgba ) {
	syscall( UI_R_SETCOLOR, rgba );
}

void trap_R_DrawStretchPic( float x, float y, float w, float h,
                            float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	syscall( UI_R_DRAWSTRETCHPIC, PASSFLOAT( x ), PASSFLOAT( y ), PASSFLOAT( w ), PASSFLOAT( h ),
	         PASSFLOAT( s1 ), PASSFLOAT( t1 ), PASSFLOAT( s2 ), PASSFLOAT( t2 ), hShader );
}

void trap_UpdateScreen( void ) {
	syscall( UI_UPDATESCREEN );
}

void trap_GetGlconfig( glconfig_t *glconfig ) {
	syscall( UI_GETGLCONFIG, glconfig );
}

void trap_GetClientState( uiClientState_t *state ) {
	syscall( UI_GETCLIENTSTATE, state );
}

void trap_GetConfigString( int index, char *buff, int buffsize ) {
	syscall( UI_GETCONFIGSTRING, index, buff, buffsize );
}

int trap_Key_GetCatcher( void ) {
	return syscall( UI_KEY_GETCATCHER );
}

void trap_Key_SetCatcher( int catcher ) {
	syscall( UI_KEY_SETCATCHER, catcher );
}

void trap_Key_ClearStates( void ) {
	syscall( UI_KEY_CLEARSTATES );
}

int trap_Key_IsDown( int keynum ) {
	return syscall( UI_KEY_ISDOWN, keynum );
}
