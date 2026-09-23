/*
===========================================================================
Copyright (C) 2006 Tony J. White (tjw@tjw.org)

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#ifndef __CL_HTTP_H__
#define __CL_HTTP_H__

#include "../qcommon/q_shared.h"

qboolean CL_HTTP_Init( void );
qboolean CL_HTTP_Available( void );
void CL_HTTP_Shutdown( void );
void CL_HTTP_BeginDownload( const char *remoteURL );
qboolean CL_HTTP_PerformDownload( void );

/*
===========================================================================
catfight: request/response HTTP, for talking to the matchmaker.

The download path above streams a file to disk and blocks the caller until it
is finished. Matchmaking needs the opposite shape -- small request, small
response, and above all *no hitch*, because it polls while the player is
sitting on a menu.

Each request runs on its own worker thread performing an ordinary blocking
transfer, which is far simpler than curl_multi or WinINet's async callbacks and
is indistinguishable from the game loop's point of view. Poll the state each
frame; the response is only valid once the state has left RUNNING.
===========================================================================
*/

#define HTTP_MAX_RESPONSE  8192
#define HTTP_MAX_URL       512
// Sized for the largest body this client sends, which is an auth request
// carrying a Steam web-api ticket: about a kilobyte of binary, so two
// kilobytes of hex, plus the JSON around it. It was 2048, which a ticket
// alone exactly filled -- see the overflow check in CL_HTTP_BeginRequest for
// why that was worse than it sounds.
#define HTTP_MAX_BODY      9216
#define HTTP_MAX_HEADERS   512
#define HTTP_MAX_ERROR     256

typedef enum {
	HTTPREQ_INVALID,	// no such handle
	HTTPREQ_RUNNING,
	HTTPREQ_DONE,		// transport succeeded; check the HTTP status code
	HTTPREQ_FAILED		// transport failed; see CL_HTTP_RequestError
} httpReqState_t;

// Returns a handle (>0) or 0 if HTTP is unavailable or every slot is busy.
// `headers` is a CRLF-separated list, or NULL. `body` may be NULL for GET.
int             CL_HTTP_BeginRequest( const char *method, const char *url,
                                      const char *body, const char *headers );
httpReqState_t  CL_HTTP_RequestState( int handle );
int             CL_HTTP_RequestCode( int handle );	// HTTP status, 0 if none
const char     *CL_HTTP_RequestBody( int handle );	// NUL-terminated, never NULL
const char     *CL_HTTP_RequestError( int handle );
void            CL_HTTP_FreeRequest( int handle );

// Releases any slot whose worker has finished and been abandoned. Cheap; call
// once a frame.
void            CL_HTTP_ReapRequests( void );

/*
Platform backends implement exactly this. It blocks, and it is only ever called
on a worker thread. A 4xx/5xx is a *successful* transfer: report qtrue and set
code, so the caller can tell "the matchmaker said no" apart from "the
matchmaker is unreachable".
*/
qboolean Sys_HTTP_Transfer( const char *method, const char *url,
                            const char *body, const char *headers,
                            int *code, char *out, int outSize, int *outLen,
                            char *err, int errSize );

// Called by the platform CL_HTTP_Init/CL_HTTP_Shutdown.
void CL_HTTP_ReqInit( void );
void CL_HTTP_ReqShutdown( void );

#endif	// __CL_HTTP_H__
