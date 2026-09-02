/*
===========================================================================
catfight -- client game module syscall glue.

ABI plumbing over the engine's cgame import table.
===========================================================================
*/

#include "cg_local.h"

static intptr_t ( QDECL *syscall )( intptr_t arg, ... ) = ( intptr_t ( QDECL * )( intptr_t, ... ) ) - 1;

Q_EXPORT void dllEntry( intptr_t ( QDECL *syscallptr )( intptr_t arg, ... ) ) {
	syscall = syscallptr;
}

static int PASSFLOAT( float x ) {
	floatint_t fi;
	fi.f = x;
	return fi.i;
}

void trap_Print( const char *fmt ) {
	syscall( CG_PRINT, fmt );
}

void trap_Error( const char *fmt ) {
	syscall( CG_ERROR, fmt );
	exit( 1 );
}

int trap_Milliseconds( void ) {
	return syscall( CG_MILLISECONDS );
}

void trap_Cvar_Register( vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags ) {
	syscall( CG_CVAR_REGISTER, vmCvar, varName, defaultValue, flags );
}

void trap_Cvar_Update( vmCvar_t *vmCvar ) {
	syscall( CG_CVAR_UPDATE, vmCvar );
}

void trap_Cvar_Set( const char *var_name, const char *value ) {
	syscall( CG_CVAR_SET, var_name, value );
}

void trap_Cvar_VariableStringBuffer( const char *var_name, char *buffer, int bufsize ) {
	syscall( CG_CVAR_VARIABLESTRINGBUFFER, var_name, buffer, bufsize );
}

int trap_Argc( void ) {
	return syscall( CG_ARGC );
}

void trap_Argv( int n, char *buffer, int bufferLength ) {
	syscall( CG_ARGV, n, buffer, bufferLength );
}

void trap_Args( char *buffer, int bufferLength ) {
	syscall( CG_ARGS, buffer, bufferLength );
}

void trap_SendConsoleCommand( const char *text ) {
	syscall( CG_SENDCONSOLECOMMAND, text );
}

void trap_AddCommand( const char *cmdName ) {
	syscall( CG_ADDCOMMAND, cmdName );
}

void trap_RemoveCommand( const char *cmdName ) {
	syscall( CG_REMOVECOMMAND, cmdName );
}

void trap_SendClientCommand( const char *s ) {
	syscall( CG_SENDCLIENTCOMMAND, s );
}

void trap_UpdateScreen( void ) {
	syscall( CG_UPDATESCREEN );
}

void trap_CM_LoadMap( const char *mapname ) {
	syscall( CG_CM_LOADMAP, mapname );
}

int trap_CM_NumInlineModels( void ) {
	return syscall( CG_CM_NUMINLINEMODELS );
}

clipHandle_t trap_CM_InlineModel( int index ) {
	return syscall( CG_CM_INLINEMODEL, index );
}

int trap_CM_PointContents( const vec3_t p, clipHandle_t model ) {
	return syscall( CG_CM_POINTCONTENTS, p, model );
}

void trap_CM_BoxTrace( trace_t *results, const vec3_t start, const vec3_t end,
                       const vec3_t mins, const vec3_t maxs, clipHandle_t model, int brushmask ) {
	syscall( CG_CM_BOXTRACE, results, start, end, mins, maxs, model, brushmask );
}

void trap_S_ClearLoopingSounds( qboolean killall ) {
	syscall( CG_S_CLEARLOOPINGSOUNDS, killall );
}

void trap_S_Respatialize( int entityNum, const vec3_t origin, vec3_t axis[3], int inwater ) {
	syscall( CG_S_RESPATIALIZE, entityNum, origin, axis, inwater );
}

sfxHandle_t trap_S_RegisterSound( const char *sample, qboolean compressed ) {
	return syscall( CG_S_REGISTERSOUND, sample, compressed );
}

void trap_S_StartLocalSound( sfxHandle_t sfx, int channelNum ) {
	syscall( CG_S_STARTLOCALSOUND, sfx, channelNum );
}

// Positional, unlike the local one above: a gunshot has to come from where the
// person firing it is standing, because which direction a shot came from is
// most of what a player has to go on.
void trap_S_StartSound( const vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfx ) {
	syscall( CG_S_STARTSOUND, origin, entityNum, entchannel, sfx );
}

void trap_R_LoadWorldMap( const char *mapname ) {
	syscall( CG_R_LOADWORLDMAP, mapname );
}

qhandle_t trap_R_RegisterModel( const char *name ) {
	return syscall( CG_R_REGISTERMODEL, name );
}

qhandle_t trap_R_RegisterShader( const char *name ) {
	return syscall( CG_R_REGISTERSHADER, name );
}

qhandle_t trap_R_RegisterShaderNoMip( const char *name ) {
	return syscall( CG_R_REGISTERSHADERNOMIP, name );
}

void trap_R_ClearScene( void ) {
	syscall( CG_R_CLEARSCENE );
}

void trap_R_AddRefEntityToScene( const refEntity_t *re ) {
	syscall( CG_R_ADDREFENTITYTOSCENE, re );
}

void trap_R_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b ) {
	syscall( CG_R_ADDLIGHTTOSCENE, org, PASSFLOAT( intensity ),
	         PASSFLOAT( r ), PASSFLOAT( g ), PASSFLOAT( b ) );
}

void trap_R_RenderScene( const refdef_t *fd ) {
	syscall( CG_R_RENDERSCENE, fd );
}

void trap_R_SetColor( const float *rgba ) {
	syscall( CG_R_SETCOLOR, rgba );
}

void trap_R_DrawStretchPic( float x, float y, float w, float h,
                            float s1, float t1, float s2, float t2, qhandle_t hShader ) {
	syscall( CG_R_DRAWSTRETCHPIC, PASSFLOAT( x ), PASSFLOAT( y ), PASSFLOAT( w ), PASSFLOAT( h ),
	         PASSFLOAT( s1 ), PASSFLOAT( t1 ), PASSFLOAT( s2 ), PASSFLOAT( t2 ), hShader );
}

void trap_GetGlconfig( glconfig_t *glconfig ) {
	syscall( CG_GETGLCONFIG, glconfig );
}

void trap_GetGameState( gameState_t *gamestate ) {
	syscall( CG_GETGAMESTATE, gamestate );
}

void trap_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime ) {
	syscall( CG_GETCURRENTSNAPSHOTNUMBER, snapshotNumber, serverTime );
}

qboolean trap_GetSnapshot( int snapshotNumber, snapshot_t *snapshot ) {
	return syscall( CG_GETSNAPSHOT, snapshotNumber, snapshot );
}

qboolean trap_GetServerCommand( int serverCommandNumber ) {
	return syscall( CG_GETSERVERCOMMAND, serverCommandNumber );
}

int trap_GetCurrentCmdNumber( void ) {
	return syscall( CG_GETCURRENTCMDNUMBER );
}

qboolean trap_GetUserCmd( int cmdNumber, usercmd_t *ucmd ) {
	return syscall( CG_GETUSERCMD, cmdNumber, ucmd );
}

void trap_SetUserCmdValue( int stateValue, float sensitivityScale ) {
	syscall( CG_SETUSERCMDVALUE, stateValue, PASSFLOAT( sensitivityScale ) );
}

int trap_Key_IsDown( int keynum ) {
	return syscall( CG_KEY_ISDOWN, keynum );
}

int trap_Key_GetCatcher( void ) {
	return syscall( CG_KEY_GETCATCHER );
}

void trap_Key_SetCatcher( int catcher ) {
	syscall( CG_KEY_SETCATCHER, catcher );
}

int trap_RealTime( qtime_t *qtime ) {
	return syscall( CG_REAL_TIME, qtime );
}
