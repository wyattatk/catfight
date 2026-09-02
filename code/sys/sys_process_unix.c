/*
===========================================================================
catfight

Child processes with pipes, POSIX backend. See the interface comment in
qcommon.h for why this exists at all.

The only client of this today is the Steam helper. Nothing here knows that.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

struct sysProcess_s {
	pid_t	pid;
	int		toChild;	// our write end of the child's stdin
	int		fromChild;	// our read end of the child's stdout
	qboolean reaped;
};

static void CloseIfOpen( int *fd ) {
	if ( *fd >= 0 ) {
		close( *fd );
	}
	*fd = -1;
}

sysProcess_t *Sys_StartProcess( const char *name ) {
	sysProcess_t	*proc;
	char			path[MAX_OSPATH];
	int				in[2], out[2];
	pid_t			pid;

	Q_strncpyz( path, Sys_BinaryPathRelative( name ), sizeof( path ) );

	if ( access( path, X_OK ) != 0 ) {
		// Not an error worth shouting about: a build without the helper is a
		// legitimate build, and the caller decides what to do without it.
		return NULL;
	}

	// Writing to a pipe whose reader has died raises SIGPIPE, which by default
	// kills the game. We want the EPIPE return instead, everywhere -- there is
	// nothing in this engine that wants to die on a broken pipe.
	signal( SIGPIPE, SIG_IGN );

	if ( pipe( in ) != 0 ) {
		return NULL;
	}
	if ( pipe( out ) != 0 ) {
		close( in[0] );
		close( in[1] );
		return NULL;
	}

	pid = fork();
	if ( pid < 0 ) {
		close( in[0] );  close( in[1] );
		close( out[0] ); close( out[1] );
		return NULL;
	}

	if ( pid == 0 ) {
		// Child. Only async-signal-safe calls until execv, so nothing here
		// prints or allocates -- a failure is reported by exiting, and the
		// parent sees it as EOF on the first read.
		char *argv[2];

		dup2( in[0], STDIN_FILENO );
		dup2( out[1], STDOUT_FILENO );
		// stderr is deliberately left alone rather than pointed at the same
		// pipe: the child's diagnostics would otherwise interleave with the
		// protocol stream and corrupt it.

		close( in[0] );  close( in[1] );
		close( out[0] ); close( out[1] );

		argv[0] = path;
		argv[1] = NULL;
		execv( path, argv );
		_exit( 127 );
	}

	// Parent. Our copies of the child's ends must go, or the child never reads
	// EOF on stdin and we never see EOF on its stdout.
	close( in[0] );
	close( out[1] );

	// The frame loop pumps this; a blocking read on a child waiting for Valve
	// would freeze the game.
	fcntl( out[0], F_SETFL, fcntl( out[0], F_GETFL, 0 ) | O_NONBLOCK );

	proc = Z_Malloc( sizeof( *proc ) );
	proc->pid = pid;
	proc->toChild = in[1];
	proc->fromChild = out[0];
	proc->reaped = qfalse;
	return proc;
}

// Reaps without blocking, so a child that has exited does not linger as a
// zombie for the life of the game.
static void Sys_ProcessPoll( sysProcess_t *proc ) {
	int status;

	if ( proc->reaped || proc->pid <= 0 ) {
		return;
	}
	if ( waitpid( proc->pid, &status, WNOHANG ) == proc->pid ) {
		proc->reaped = qtrue;
	}
}

void Sys_StopProcess( sysProcess_t *proc ) {
	int tries;

	if ( !proc ) {
		return;
	}

	// Closing stdin is the polite request: a well-behaved child reads EOF and
	// exits. Give it a moment before insisting.
	CloseIfOpen( &proc->toChild );

	for ( tries = 0; tries < 20 && !proc->reaped; tries++ ) {
		Sys_ProcessPoll( proc );
		if ( !proc->reaped ) {
			Sys_Sleep( 100 );
		}
	}
	if ( !proc->reaped && proc->pid > 0 ) {
		kill( proc->pid, SIGKILL );
		waitpid( proc->pid, NULL, 0 );
	}

	CloseIfOpen( &proc->fromChild );
	Z_Free( proc );
}

qboolean Sys_ProcessAlive( sysProcess_t *proc ) {
	if ( !proc || proc->pid <= 0 ) {
		return qfalse;
	}
	Sys_ProcessPoll( proc );
	return (qboolean)!proc->reaped;
}

int Sys_ProcessWrite( sysProcess_t *proc, const char *data, int len ) {
	ssize_t written;

	if ( !proc || proc->toChild < 0 ) {
		return -1;
	}
	written = write( proc->toChild, data, len );
	if ( written < 0 ) {
		return -1;
	}
	return (int)written;
}

int Sys_ProcessRead( sysProcess_t *proc, char *buf, int len ) {
	ssize_t got;

	if ( !proc || proc->fromChild < 0 ) {
		return -1;
	}
	got = read( proc->fromChild, buf, len );
	if ( got > 0 ) {
		return (int)got;
	}
	if ( got == 0 ) {
		return -1;	// EOF: the write end is gone, so the child is
	}
	if ( errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ) {
		return 0;
	}
	return -1;
}
