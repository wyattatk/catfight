/*
===========================================================================
catfight

Minimal read-only JSON accessors. See cf_json.h for why this is not jsmn.
===========================================================================
*/

#include "cf_json.h"

// Advance past whitespace.
static const char *SkipSpace( const char *p ) {
	while ( *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ) {
		p++;
	}
	return p;
}

/*
================
SkipString

p points at the opening quote. Returns just past the closing quote, or NULL on
an unterminated string. Escapes are honoured so that a \" does not end it.
================
*/
static const char *SkipString( const char *p ) {
	if ( *p != '"' ) {
		return NULL;
	}
	p++;
	while ( *p ) {
		if ( *p == '\\' ) {
			if ( !p[1] ) {
				return NULL;
			}
			p += 2;
			continue;
		}
		if ( *p == '"' ) {
			return p + 1;
		}
		p++;
	}
	return NULL;
}

/*
================
SkipValue

Returns just past a complete value of any type. Objects and arrays are walked
with a depth counter so that nested content cannot be mistaken for the level
above it -- which is the whole reason a naive strstr for a key is wrong.
================
*/
static const char *SkipValue( const char *p ) {
	int depth;

	p = SkipSpace( p );
	if ( !*p ) {
		return NULL;
	}

	if ( *p == '"' ) {
		return SkipString( p );
	}

	if ( *p == '{' || *p == '[' ) {
		depth = 0;
		while ( *p ) {
			if ( *p == '"' ) {
				p = SkipString( p );
				if ( !p ) {
					return NULL;
				}
				continue;
			}
			if ( *p == '{' || *p == '[' ) {
				depth++;
			} else if ( *p == '}' || *p == ']' ) {
				depth--;
				if ( depth == 0 ) {
					return p + 1;
				}
			}
			p++;
		}
		return NULL;
	}

	// number, true, false, null
	while ( *p && *p != ',' && *p != '}' && *p != ']' &&
			*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' ) {
		p++;
	}
	return p;
}

/*
================
UnescapeInto

Copies a JSON string body (p is just past the opening quote) into out,
resolving escapes. \u is decoded only for the ASCII range; anything else
becomes '?', which is honest about what happened rather than emitting broken
bytes into a console line.
================
*/
static qboolean UnescapeInto( const char *p, char *out, int outSize ) {
	int written = 0;

	while ( *p && *p != '"' ) {
		char c;

		if ( written >= outSize - 1 ) {
			return qfalse;
		}

		if ( *p != '\\' ) {
			out[written++] = *p++;
			continue;
		}

		p++;
		switch ( *p ) {
			case '"':  c = '"';  break;
			case '\\': c = '\\'; break;
			case '/':  c = '/';  break;
			case 'b':  c = '\b'; break;
			case 'f':  c = '\f'; break;
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
			case 'u': {
				int i, code = 0;
				for ( i = 1; i <= 4; i++ ) {
					char h = p[i];
					int  d;
					if ( h >= '0' && h <= '9' )      d = h - '0';
					else if ( h >= 'a' && h <= 'f' ) d = h - 'a' + 10;
					else if ( h >= 'A' && h <= 'F' ) d = h - 'A' + 10;
					else return qfalse;
					code = ( code << 4 ) | d;
				}
				p += 4;
				c = ( code >= 0x20 && code < 0x7f ) ? (char)code : '?';
				break;
			}
			case '\0':
				return qfalse;
			default:
				return qfalse;
		}
		out[written++] = c;
		p++;
	}

	if ( *p != '"' ) {
		return qfalse;   // unterminated
	}

	out[written] = '\0';
	return qtrue;
}

/*
================
FindTopLevelValue

Returns a pointer to the start of the value for key, at depth 1 only, or NULL.
================
*/
static const char *FindTopLevelValue( const char *json, const char *key ) {
	const char *p;
	int keyLen;

	if ( !json || !key ) {
		return NULL;
	}
	keyLen = (int)strlen( key );

	p = SkipSpace( json );
	if ( *p != '{' ) {
		return NULL;
	}
	p++;

	for ( ;; ) {
		const char *keyStart, *afterKey;

		p = SkipSpace( p );
		if ( *p == '}' || !*p ) {
			return NULL;
		}
		if ( *p == ',' ) {
			p++;
			continue;
		}
		if ( *p != '"' ) {
			return NULL;   // malformed
		}

		keyStart = p + 1;
		afterKey = SkipString( p );
		if ( !afterKey ) {
			return NULL;
		}

		p = SkipSpace( afterKey );
		if ( *p != ':' ) {
			return NULL;
		}
		p = SkipSpace( p + 1 );

		// Compare without unescaping: our keys are plain identifiers, and a
		// key that needs escaping is not one of ours.
		if ( (int)( afterKey - 1 - keyStart ) == keyLen &&
			 !Q_strncmp( keyStart, key, keyLen ) ) {
			return p;
		}

		p = SkipValue( p );
		if ( !p ) {
			return NULL;
		}
	}
}

qboolean CF_JSON_GetString( const char *json, const char *key, char *out, int outSize ) {
	const char *v;

	if ( !out || outSize <= 0 ) {
		return qfalse;
	}
	out[0] = '\0';

	v = FindTopLevelValue( json, key );
	if ( !v ) {
		return qfalse;
	}

	if ( *v == '"' ) {
		return UnescapeInto( v + 1, out, outSize );
	}

	if ( *v == '{' || *v == '[' ) {
		return qfalse;   // caller wanted a scalar
	}

	{
		// Bare literal: copy it verbatim.
		const char *end = SkipValue( v );
		int len;

		if ( !end ) {
			return qfalse;
		}
		len = (int)( end - v );
		if ( len <= 0 || len >= outSize ) {
			return qfalse;
		}
		memcpy( out, v, len );
		out[len] = '\0';
		return qtrue;
	}
}

qboolean CF_JSON_GetInt( const char *json, const char *key, int *out ) {
	char buf[32];

	if ( !out ) {
		return qfalse;
	}
	if ( !CF_JSON_GetString( json, key, buf, sizeof( buf ) ) ) {
		return qfalse;
	}
	if ( !buf[0] ) {
		return qfalse;
	}

	*out = atoi( buf );
	return qtrue;
}
