/*
===========================================================================
catfight

Minimal read-only JSON accessors, enough for the matchmaker API.
===========================================================================
*/

#ifndef __CF_JSON_H__
#define __CF_JSON_H__

#include "q_shared.h"

/*
This is deliberately NOT a general JSON parser.

netplay/README.md planned to vendor jsmn, and that is still the right answer
the day this needs to walk arrays or nested objects. It does not yet: every
response cfmm sends the client is a flat object of strings and numbers --

    {"state":"matched","match_id":3,"team":0,"ticket":"...","connect":"..."}

-- and a focused reader for exactly that is a fraction of the size, carries no
third-party licence, and cannot silently half-work on input it was never meant
to handle. Nested values are *skipped* correctly rather than parsed, so a
lookup never wanders into a sub-object and returns something plausible from the
wrong level.

When the API grows arrays, replace this with jsmn rather than extending it.
*/

// Look up a top-level key. Returns qfalse if absent, if the value is an object
// or array, or if the output buffer is too small.
qboolean CF_JSON_GetString( const char *json, const char *key, char *out, int outSize );

// As above for integers. Accepts a bare number or a quoted one.
qboolean CF_JSON_GetInt( const char *json, const char *key, int *out );

#endif // __CF_JSON_H__
