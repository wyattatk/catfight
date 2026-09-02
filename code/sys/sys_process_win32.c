/*
===========================================================================
catfight

Child processes with pipes, Windows backend. See the interface comment in
qcommon.h for why this exists at all.

The only client of this today is the Steam helper. Nothing here knows that.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#include <windows.h>

struct sysProcess_s {
	HANDLE	process;
	HANDLE	toChild;	// our write end of the child's stdin
	HANDLE	fromChild;	// our read end of the child's stdout
};

static void CloseIfValid( HANDLE *h ) {
	if ( *h && *h != INVALID_HANDLE_VALUE ) {
		CloseHandle( *h );
	}
	*h = NULL;
}

sysProcess_t *Sys_StartProcess( const char *name ) {
	SECURITY_ATTRIBUTES	sa;
	STARTUPINFO			si;
	PROCESS_INFORMATION	pi;
	sysProcess_t		*proc;
	HANDLE				childStdinRead = NULL, childStdinWrite = NULL;
	HANDLE				childStdoutRead = NULL, childStdoutWrite = NULL;
	char				path[MAX_OSPATH];
	char				cmdline[MAX_OSPATH + 2];

	Q_strncpyz( path, Sys_BinaryPathRelative( name ), sizeof( path ) );
	Q_strcat( path, sizeof( path ), ".exe" );

	if ( GetFileAttributes( path ) == INVALID_FILE_ATTRIBUTES ) {
		// Not an error worth shouting about: a build without the helper is a
		// legitimate build, and the caller decides what to do without it.
		return NULL;
	}

	// The pipe ends we hand the child must be inheritable; the ends we keep
	// must NOT be, or the child holds a copy of our read end and we never see
	// EOF when it dies.
	sa.nLength = sizeof( sa );
	sa.lpSecurityDescriptor = NULL;
	sa.bInheritHandle = TRUE;

	if ( !CreatePipe( &childStdinRead, &childStdinWrite, &sa, 0 ) ) {
		return NULL;
	}
	if ( !CreatePipe( &childStdoutRead, &childStdoutWrite, &sa, 0 ) ) {
		CloseIfValid( &childStdinRead );
		CloseIfValid( &childStdinWrite );
		return NULL;
	}
	SetHandleInformation( childStdinWrite, HANDLE_FLAG_INHERIT, 0 );
	SetHandleInformation( childStdoutRead, HANDLE_FLAG_INHERIT, 0 );

	Com_Memset( &si, 0, sizeof( si ) );
	si.cb = sizeof( si );
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = childStdinRead;
	si.hStdOutput = childStdoutWrite;
	// Deliberately NOT the same pipe as stdout. The child's diagnostics would
	// otherwise interleave with the protocol stream and corrupt it.
	si.hStdError = GetStdHandle( STD_ERROR_HANDLE );

	// CreateProcess may write to its command line argument, so it cannot be a
	// literal or the path buffer we still need.
	Com_sprintf( cmdline, sizeof( cmdline ), "\"%s\"", path );

	Com_Memset( &pi, 0, sizeof( pi ) );
	if ( !CreateProcess( path, cmdline, NULL, NULL, TRUE,
						 CREATE_NO_WINDOW, NULL, NULL, &si, &pi ) ) {
		Com_Printf( S_COLOR_YELLOW "could not start %s (error %lu)\n",
					path, (unsigned long)GetLastError() );
		CloseIfValid( &childStdinRead );
		CloseIfValid( &childStdinWrite );
		CloseIfValid( &childStdoutRead );
		CloseIfValid( &childStdoutWrite );
		return NULL;
	}

	// Our copies of the child's ends. Holding them open would mean the child
	// never reads EOF on stdin and never exits when we let go.
	CloseIfValid( &childStdinRead );
	CloseIfValid( &childStdoutWrite );
	CloseHandle( pi.hThread );

	proc = Z_Malloc( sizeof( *proc ) );
	proc->process = pi.hProcess;
	proc->toChild = childStdinWrite;
	proc->fromChild = childStdoutRead;
	return proc;
}

void Sys_StopProcess( sysProcess_t *proc ) {
	if ( !proc ) {
		return;
	}

	// Closing stdin is the polite request: a well-behaved child reads EOF and
	// exits. Give it a moment before insisting.
	CloseIfValid( &proc->toChild );

	if ( proc->process ) {
		if ( WaitForSingleObject( proc->process, 2000 ) != WAIT_OBJECT_0 ) {
			TerminateProcess( proc->process, 0 );
		}
		CloseIfValid( &proc->process );
	}
	CloseIfValid( &proc->fromChild );
	Z_Free( proc );
}

qboolean Sys_ProcessAlive( sysProcess_t *proc ) {
	if ( !proc || !proc->process ) {
		return qfalse;
	}
	return WaitForSingleObject( proc->process, 0 ) == WAIT_TIMEOUT;
}

int Sys_ProcessWrite( sysProcess_t *proc, const char *data, int len ) {
	DWORD written = 0;

	if ( !proc || !proc->toChild ) {
		return -1;
	}
	if ( !WriteFile( proc->toChild, data, (DWORD)len, &written, NULL ) ) {
		return -1;
	}
	return (int)written;
}

int Sys_ProcessRead( sysProcess_t *proc, char *buf, int len ) {
	DWORD available = 0, got = 0;

	if ( !proc || !proc->fromChild ) {
		return -1;
	}

	// Peek first. ReadFile on a pipe blocks until at least one byte arrives,
	// and this is called from the frame loop.
	if ( !PeekNamedPipe( proc->fromChild, NULL, 0, NULL, &available, NULL ) ) {
		return -1;	// the write end is gone, so the child is
	}
	if ( available == 0 ) {
		return 0;
	}
	if ( available > (DWORD)len ) {
		available = (DWORD)len;
	}
	if ( !ReadFile( proc->fromChild, buf, available, &got, NULL ) ) {
		return -1;
	}
	return (int)got;
}
