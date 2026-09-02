/*
===========================================================================
catfight

Child processes with pipes, for platforms that do not have any.

Emscripten is the one that matters today: a web build has no process model at
all. Rather than making every caller conditional, the platform simply says no
and the caller's existing "no helper" path handles it -- which is a path that
must work anyway, because a desktop build without the Steam helper installed
takes exactly the same one.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

sysProcess_t *Sys_StartProcess( const char *name ) {
	return NULL;
}

void Sys_StopProcess( sysProcess_t *proc ) {
}

qboolean Sys_ProcessAlive( sysProcess_t *proc ) {
	return qfalse;
}

int Sys_ProcessWrite( sysProcess_t *proc, const char *data, int len ) {
	return -1;
}

int Sys_ProcessRead( sysProcess_t *proc, char *buf, int len ) {
	return -1;
}
