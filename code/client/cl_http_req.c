/*
===========================================================================
catfight

Request/response HTTP for the matchmaker. Platform-independent half: slots,
worker threads and lifetime. The blocking transfer itself is Sys_HTTP_Transfer,
implemented per platform in cl_http_windows.c and cl_http_curl.c.
===========================================================================
*/

#ifdef USE_HTTP

#include "client.h"

#ifdef USE_INTERNAL_SDL_HEADERS
#	include "SDL.h"
#	include "SDL_thread.h"
#else
#	include <SDL.h>
#	include <SDL_thread.h>
#endif

// Matchmaking issues one request at a time; the rest is headroom so that a
// slow request in flight never blocks a cancel from being sent.
#define MAX_HTTP_REQUESTS 4

typedef struct {
	qboolean		used;
	int				handle;			// 0 when free; unique and monotonic otherwise
	httpReqState_t	state;
	qboolean		abandoned;		// freed by the caller while still running

	char			method[8];
	char			url[HTTP_MAX_URL];
	char			body[HTTP_MAX_BODY];
	char			headers[HTTP_MAX_HEADERS];

	int				code;
	char			response[HTTP_MAX_RESPONSE];
	int				responseLen;
	char			error[HTTP_MAX_ERROR];

	SDL_Thread		*thread;
} httpRequest_t;

static httpRequest_t	httpRequests[MAX_HTTP_REQUESTS];
static SDL_mutex		*httpMutex = NULL;
static int				httpNextHandle = 1;

/*
================
CL_HTTP_ReqInit

Called from the platform CL_HTTP_Init. Safe to call repeatedly.
================
*/
void CL_HTTP_ReqInit( void ) {
	if ( !httpMutex ) {
		httpMutex = SDL_CreateMutex();
	}
}

static httpRequest_t *FindRequest( int handle ) {
	int i;

	if ( handle <= 0 ) {
		return NULL;
	}
	for ( i = 0; i < MAX_HTTP_REQUESTS; i++ ) {
		if ( httpRequests[i].used && httpRequests[i].handle == handle ) {
			return &httpRequests[i];
		}
	}
	return NULL;
}

/*
================
ReleaseSlot

Joins the worker and clears the slot. Must be called with the mutex NOT held --
SDL_WaitThread can block, and the worker takes the mutex on its way out.
================
*/
static void ReleaseSlot( httpRequest_t *req ) {
	SDL_Thread *thread = req->thread;

	req->thread = NULL;
	if ( thread ) {
		SDL_WaitThread( thread, NULL );
	}

	SDL_LockMutex( httpMutex );
	Com_Memset( req, 0, sizeof( *req ) );
	SDL_UnlockMutex( httpMutex );
}

/*
================
HTTP_Worker

One blocking transfer. Everything it touches belongs to its own slot, and the
state write is last and under the mutex, so the main thread never sees a
half-written response.
================
*/
static int SDLCALL HTTP_Worker( void *data ) {
	httpRequest_t *req = (httpRequest_t *)data;
	char		response[HTTP_MAX_RESPONSE];
	char		error[HTTP_MAX_ERROR];
	int			code = 0, len = 0;
	qboolean	ok;

	response[0] = '\0';
	error[0] = '\0';

	ok = Sys_HTTP_Transfer( req->method, req->url,
							req->body[0] ? req->body : NULL,
							req->headers[0] ? req->headers : NULL,
							&code, response, sizeof( response ), &len,
							error, sizeof( error ) );

	SDL_LockMutex( httpMutex );
	req->code = code;
	req->responseLen = len;
	if ( len > 0 && len < (int)sizeof( req->response ) ) {
		Com_Memcpy( req->response, response, len );
		req->response[len] = '\0';
	} else {
		req->response[0] = '\0';
		req->responseLen = 0;
	}
	Q_strncpyz( req->error, error, sizeof( req->error ) );
	req->state = ok ? HTTPREQ_DONE : HTTPREQ_FAILED;
	SDL_UnlockMutex( httpMutex );

	return 0;
}

int CL_HTTP_BeginRequest( const char *method, const char *url,
						  const char *body, const char *headers ) {
	httpRequest_t *req = NULL;
	int i, handle;

	if ( !CL_HTTP_Available() ) {
		return 0;
	}
	CL_HTTP_ReqInit();
	if ( !httpMutex ) {
		return 0;
	}
	if ( !url || !*url ) {
		return 0;
	}

	/*
	Refuse an oversized body rather than truncating it.

	This used to be a plain Q_strncpyz into a fixed buffer, and the first thing
	that outgrew it -- a Steam ticket -- was cut mid-string. What arrives at the
	other end is not a short request, it is INVALID JSON, and the server answers
	"malformed body: unexpected EOF". That sends you to look at how the body is
	built, which is the one place the bug is not.
	*/
	if ( body && strlen( body ) >= HTTP_MAX_BODY ) {
		Com_Printf( S_COLOR_YELLOW "CL_HTTP_BeginRequest: body is %d bytes, "
					"the limit is %d\n", (int)strlen( body ), HTTP_MAX_BODY - 1 );
		return 0;
	}

	CL_HTTP_ReapRequests();

	SDL_LockMutex( httpMutex );
	for ( i = 0; i < MAX_HTTP_REQUESTS; i++ ) {
		if ( !httpRequests[i].used ) {
			req = &httpRequests[i];
			break;
		}
	}
	if ( !req ) {
		SDL_UnlockMutex( httpMutex );
		Com_DPrintf( "CL_HTTP_BeginRequest: no free slot\n" );
		return 0;
	}

	Com_Memset( req, 0, sizeof( *req ) );
	req->used = qtrue;
	req->handle = handle = httpNextHandle++;
	req->state = HTTPREQ_RUNNING;
	Q_strncpyz( req->method, method ? method : "GET", sizeof( req->method ) );
	Q_strncpyz( req->url, url, sizeof( req->url ) );
	if ( body ) {
		Q_strncpyz( req->body, body, sizeof( req->body ) );
	}
	if ( headers ) {
		Q_strncpyz( req->headers, headers, sizeof( req->headers ) );
	}
	SDL_UnlockMutex( httpMutex );

	req->thread = SDL_CreateThread( HTTP_Worker, "cf_http", req );
	if ( !req->thread ) {
		SDL_LockMutex( httpMutex );
		Com_Memset( req, 0, sizeof( *req ) );
		SDL_UnlockMutex( httpMutex );
		Com_Printf( "CL_HTTP_BeginRequest: SDL_CreateThread failed: %s\n",
					SDL_GetError() );
		return 0;
	}

	return handle;
}

httpReqState_t CL_HTTP_RequestState( int handle ) {
	httpRequest_t *req;
	httpReqState_t state;

	if ( !httpMutex ) {
		return HTTPREQ_INVALID;
	}
	SDL_LockMutex( httpMutex );
	req = FindRequest( handle );
	state = req ? req->state : HTTPREQ_INVALID;
	SDL_UnlockMutex( httpMutex );

	return state;
}

int CL_HTTP_RequestCode( int handle ) {
	httpRequest_t *req;
	int code;

	if ( !httpMutex ) {
		return 0;
	}
	SDL_LockMutex( httpMutex );
	req = FindRequest( handle );
	code = req ? req->code : 0;
	SDL_UnlockMutex( httpMutex );

	return code;
}

/*
================
CL_HTTP_RequestBody

Safe to hold onto only until the next CL_HTTP_FreeRequest for this handle. The
worker no longer touches the buffer once the state has left RUNNING, which is
the only state a caller should be reading in.
================
*/
const char *CL_HTTP_RequestBody( int handle ) {
	httpRequest_t *req;

	if ( !httpMutex ) {
		return "";
	}
	SDL_LockMutex( httpMutex );
	req = FindRequest( handle );
	SDL_UnlockMutex( httpMutex );

	if ( !req || req->state == HTTPREQ_RUNNING ) {
		return "";
	}
	return req->response;
}

const char *CL_HTTP_RequestError( int handle ) {
	httpRequest_t *req;

	if ( !httpMutex ) {
		return "";
	}
	SDL_LockMutex( httpMutex );
	req = FindRequest( handle );
	SDL_UnlockMutex( httpMutex );

	if ( !req || req->state == HTTPREQ_RUNNING ) {
		return "";
	}
	return req->error;
}

/*
================
CL_HTTP_FreeRequest

Freeing a request whose worker is still running does not block: the slot is
marked abandoned and reclaimed by CL_HTTP_ReapRequests once the thread exits.
Blocking here would hand the player a frozen client every time they cancelled a
search against an unreachable matchmaker -- exactly when it matters most.
================
*/
void CL_HTTP_FreeRequest( int handle ) {
	httpRequest_t *req;
	qboolean running;

	if ( !httpMutex ) {
		return;
	}

	SDL_LockMutex( httpMutex );
	req = FindRequest( handle );
	if ( !req ) {
		SDL_UnlockMutex( httpMutex );
		return;
	}
	running = ( req->state == HTTPREQ_RUNNING );
	if ( running ) {
		req->abandoned = qtrue;
	}
	SDL_UnlockMutex( httpMutex );

	if ( !running ) {
		ReleaseSlot( req );
	}
}

void CL_HTTP_ReapRequests( void ) {
	int i;

	if ( !httpMutex ) {
		return;
	}

	for ( i = 0; i < MAX_HTTP_REQUESTS; i++ ) {
		httpRequest_t *req = &httpRequests[i];
		qboolean reap;

		SDL_LockMutex( httpMutex );
		reap = ( req->used && req->abandoned && req->state != HTTPREQ_RUNNING );
		SDL_UnlockMutex( httpMutex );

		if ( reap ) {
			ReleaseSlot( req );
		}
	}
}

/*
================
CL_HTTP_ReqShutdown

Joins every outstanding worker. Called from the platform CL_HTTP_Shutdown,
because a worker still writing into a static slot after the subsystem is gone
is a crash on exit that only shows up on slow networks.
================
*/
void CL_HTTP_ReqShutdown( void ) {
	int i;

	if ( !httpMutex ) {
		return;
	}

	for ( i = 0; i < MAX_HTTP_REQUESTS; i++ ) {
		if ( httpRequests[i].used ) {
			ReleaseSlot( &httpRequests[i] );
		}
	}

	SDL_DestroyMutex( httpMutex );
	httpMutex = NULL;
}

#endif /* USE_HTTP */
