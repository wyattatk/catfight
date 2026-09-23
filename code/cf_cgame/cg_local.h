/*
===========================================================================
catfight -- client game module, internal header.

cgame draws the world and predicts the local player. It never decides anything:
every fact it shows comes from a server snapshot, and every prediction it makes
gets overwritten the moment the real answer arrives.
===========================================================================
*/

#ifndef CF_CG_LOCAL_H
#define CF_CG_LOCAL_H

#include "../cf_shared/cf_shared.h"
#include "../cf_shared/cf_weapons.h"
#include "../renderercommon/tr_types.h"
#include "../cgame/cg_public.h"
#include "../client/keycodes.h"

#define MAX_LOCAL_ENTITIES 512

// Matches MAX_VOICE_TEXT in cl_voice.c. Her replies are a sentence or two and
// bounded there; this is the same bound so nothing is lost crossing over, and
// what does not fit on screen is a drawing decision rather than a truncation.
#define MAX_SUBTITLE_TEXT  1024

typedef struct {
	entityState_t currentState;
	entityState_t nextState;
	qboolean      interpolate;
	qboolean      currentValid;

	vec3_t        lerpOrigin;
	vec3_t        lerpAngles;

	int           previousEvent;   // so one event is not fired twice

	// Set by EV_RELOAD when the slide was locked back, so EV_RELOAD_DONE knows
	// to finish with the slide slamming forward. Per entity, because two people
	// can be reloading different guns at once.
	qboolean      reloadFromEmpty;

	// cg.time of this entity's last shot, for the muzzle flash. Per entity
	// because two people can be firing at once, and the flash belongs to
	// whoever pulled the trigger.
	int           muzzleFlashTime;

	// Which body animation is playing and how far into it, in frames, for the
	// legs and separately for the torso. Per entity because every player is
	// somewhere different in their stride. See cg_players.c.
	int           anim;
	float         animPhase;
	int           torsoAnim;
	float         torsoPhase;

	// cg.time of the EV_RELOAD that started their reload and how long that kind
	// of reload takes, in ms; reloadTime is 0 when they are not reloading. The
	// torso and the magazine are both placed along it -- see CG_ReloadProgress.
	int           reloadTime;
	int           reloadTotal;

	// The last time a round landed in this body, and which way it was going
	// (flat, unit length) -- the torso flinches away from it. See
	// CG_FlinchFromImpact.
	int           flinchTime;
	vec3_t        flinchDir;
} centity_t;

/*
The body animations cgame knows how to choose between. A body names them in its
animation.cfg; the order here is only this code's, not any model's.
*/
typedef enum {
	CF_ANIM_IDLE,
	CF_ANIM_RUN,
	CF_ANIM_CROUCH,
	CF_ANIM_CROUCHWALK,
	CF_ANIM_JUMP,
	CF_ANIM_DEATH,

	// upper body only, played over whichever of the above the legs are doing
	CF_ANIM_TORSO_IDLE,
	CF_ANIM_TORSO_RELOAD,
	CF_ANIM_TORSO_AIM,     // scrubbed by view pitch, not played; see CF_AIM_PITCH

	// your own arms, seen from inside; see CG_AddViewArms
	CF_ANIM_FP_IDLE,
	CF_ANIM_FP_RELOAD,

	CF_ANIM_COUNT
} cfAnimNum_t;

/*
How far up or down torso_aim reaches, in degrees each way: its first frame is
looking this far up, its last this far down, evenly spaced between. Must agree
with AIM_PITCH in mapping/gen_body.py. Looking further than this holds the end
frame -- the arms stop, the gun (which follows the hand) stops with them.
*/
#define CF_AIM_PITCH  80.0f

// Horizontal speed above which a body is moving rather than standing. Below it
// the idle plays: a player shuffling at a few units a second does not want legs
// windmilling to keep up.
#define CF_ANIM_MOVING_SPEED  40.0f

/*
How long a shot's flash lasts: the flame at the muzzle and the light it throws.

Deliberately SHORT -- about three frames. A muzzle flash that outlasts the
shot reads as a lamp rather than an explosion.
*/
#define CF_MUZZLE_FLASH_TIME  45

/*
What we know about another player, parsed out of their CS_PLAYERS configstring.
The scoreboard is drawn entirely from these, which is why a client that joins
halfway through a match still has a complete one.
*/
typedef struct {
	qboolean infoValid;

	char     name[MAX_NETNAME];
	team_t   team;

	int      score;
	int      kills;
	int      deaths;
	qboolean eliminated;

	// Which body they are drawn with: models/players/<body>/<body>.iqm. The
	// server decides it (G_ClientBody) so every screen agrees.
	char     body[32];
} clientInfo_t;

typedef struct {
	int         clientFrame;

	int         clientNum;

	qboolean    demoPlayback;
	qboolean    levelShot;

	// the snapshot we are currently showing, and the one after it if we have
	// it -- entities are interpolated between the two
	snapshot_t *snap;
	snapshot_t *nextSnap;
	snapshot_t  activeSnapshots[2];

	float       frameInterpolation;   // 0..1 between snap and nextSnap

	qboolean    thisFrameTeleport;
	qboolean    nextFrameTeleport;

	int         frametime;            // cg.time - cg.oldTime
	int         time;                 // this is the time value that the client
	                                  // is rendering at, always <= cl.serverTime
	int         oldTime;

	int         latestSnapshotNum;    // the number of snapshots the client system has received
	int         latestSnapshotTime;   // the time of the latest snapshot the client system has received

	// prediction
	playerState_t predictedPlayerState;
	centity_t     predictedPlayerEntity;
	qboolean      validPPS;           // clear until the first call to CG_PredictPlayerState
	int           predictedErrorTime;
	vec3_t        predictedError;

	/*
	Last frame's predicted player state, kept ONLY to find our own events by
	difference. See CG_CheckPlayerstateEvents.

	It has to be the previous PREDICTED state rather than the previous
	snapshot: prediction replays the same commands from the same base every
	frame, so a shot predicted once is re-derived with the same sequence number
	on every frame until the server confirms it. Diffing against the snapshot
	would fire it again on each of those frames -- one trigger pull heard a
	dozen times.
	*/
	playerState_t oldPlayerState;
	qboolean      validOPS;           // clear until oldPlayerState means something


	// view
	refdef_t      refdef;
	vec3_t        refdefViewAngles;
	float         fov;

	/*
	View-model motion. All of it is cgame-local and none of it is replicated,
	because none of it changes where a bullet goes -- see the header of
	cg_weapon.c. A client that guessed every one of these numbers differently
	would still hit exactly what everyone else saw it hit.
	*/
	vec3_t        gunSway;       // degrees the gun is currently trailing the view
	vec3_t        gunOldAngles;  // last frame's view angles, to difference against
	qboolean      gunValid;      // clear until gunOldAngles means something
	float         gunBob;        // smoothed 0..1 bob amplitude, follows speed
	float         gunReload;     // smoothed 0..1, how far into the reload dip

	// warning shown while we have no snapshot yet
	qboolean      infoScreenShown;

	// scoreboard, held open by +scores
	qboolean      showScores;

	// What your companion last said back when you gave her an order, and when.
	// Server-sent (see cf_ack in cg_servercmds.c); shown for a couple of seconds
	// under her health bar so an order visibly lands.
	char          allyAck[64];
	int           allyAckTime;

	/*
	Her dialogue as text, because otherwise it is audio only.

	A player who cannot hear gets a companion who never says anything -- and
	she is the entire point of the game, so that is not a missing nicety, it is
	the feature being absent. The engine holds the line (see CL_Voice_Subtitle);
	this is cgame's copy of it and the clock it will be faded against.

	subtitleSeq is the ENGINE's counter as of the last frame we looked. It is
	compared, never interpreted: the two sides keep different clocks, so the
	moment it moves we stamp the arrival with cg.time, which is the clock this
	side can actually do arithmetic with.
	*/
	char          subtitle[MAX_SUBTITLE_TEXT];
	int           subtitleSeq;
	int           subtitleTime;

	// the last round result we announced, so the banner is shown once per round
	// rather than every frame it is true for
	int           announcedRound;

	// The round-end banner we last wrote to the log, so it is written once per
	// round rather than sixty times a second. Compared by content, not by
	// pointer: the spectator's banner is built with va() and is a fresh address
	// every frame.
	char          lastBanner[64];

	/*
	THE DEATH CAMERA. deathTime is cg.time of our own EV_DEATH (0 while
	alive), killer the entity number it named. deathCam is set by
	CG_CalcViewValues for the frames the camera is actually out of our head,
	and tells the rest of the frame to draw our own body and the killer's name.
	*/
	int           deathTime;
	int           killer;
	qboolean      deathCam;

	// cg.time the last shot of ours connected. Drives the hit marker, which is
	// the only confirmation a player gets that a bullet landed -- a hitscan
	// weapon has no travel time and nothing else to watch.
	int           hitMarkerTime;

	/*
	The postgame: the match is decided and we are sitting on the result until
	the server drops us. cgame holds the key catcher while this is up, so the
	two keys the prompt names are the two keys that do anything.
	*/
	qboolean      postgame;         // the prompt is up and we hold the catcher
	qboolean      postgameChosen;   // a choice was made; the disconnect is coming
} cg_t;

typedef struct {
	qhandle_t white;         // flat 2D quad, tinted at draw time
	qhandle_t charset;       // 16x16 grid bitmap font
	qhandle_t placeholder;   // stand-in marker for entities with no model yet

	/*
	The first-person weapon. It is named by the weapon table and loaded once.
	Player bodies are not here: there are several, loaded as players arrive,
	and cg_players.c keeps them.
	*/
	qhandle_t weaponModel;
	// The muzzle flash, drawn at the weapon's muzzle for CF_MUZZLE_FLASH_TIME
	// after a shot. From mapping/gen_flash.py; see CG_AddMuzzleFlash.
	qhandle_t flashModel;

	/*
	Weapon audio. These are placeholders generated by
	mapping/gen_weapon_sounds.ps1 rather than recordings, and they are here at
	all because a gun with no sound cannot be judged: report, click and the
	two reload cadences are most of what "does this feel right" is made of.
	Dropping real recordings in at the same paths needs no code change.
	*/
	sfxHandle_t fire;
	sfxHandle_t dryFire;
	sfxHandle_t magOut;
	sfxHandle_t magIn;
	sfxHandle_t slideRelease;
	sfxHandle_t impactFlesh;
	sfxHandle_t impactWall;

	/*
	The world. Four footsteps rather than one because a repeated footstep is
	the most obviously fake sound a game can make -- the ear locks onto the
	repetition long before it notices the sample is synthetic.
	*/
	sfxHandle_t footsteps[4];
	sfxHandle_t jump;
	sfxHandle_t landSoft;
	sfxHandle_t landMedium;
	sfxHandle_t landHard;
} cgMedia_t;

typedef struct {
	gameState_t gameState;
	glconfig_t  glconfig;
	float       screenXScale;
	float       screenYScale;

	int         serverCommandSequence;
	int         processedSnapshotNum;

	char        mapname[MAX_QPATH];

	int         levelStartTime;

	// match state, mirrored from CS_MATCH_STATE / CS_SCORES
	matchState_t matchState;
	int          matchStateTime;
	int          matchStateEnd;

	int          roundNumber;
	int          teamRounds[TEAM_NUM_TEAMS];
	int          roundLimit;   // rounds to win the match; 0 if the server did not say
	int          maxRounds;    // hard cap on rounds played; 0 if uncapped or not said

	// Who took the round that just ended: a team_t, TEAM_FREE for a draw, or -1
	// when nothing has been decided yet or the server never said. TEAM_FREE is
	// a real answer here, so the unknown case cannot be zero.
	int          lastRoundWinner;

	clientInfo_t clientinfo[MAX_CLIENTS];

	cgMedia_t   media;
} cgs_t;

extern cg_t  cg;
extern cgs_t cgs;
extern centity_t cg_entities[MAX_GENTITIES];

extern vmCvar_t cg_fov;
extern vmCvar_t cg_drawCrosshair;
extern vmCvar_t cg_nopredict;
extern vmCvar_t cg_showSpeed;
extern vmCvar_t cg_forceBody;
extern vmCvar_t cg_animStride;
extern vmCvar_t cg_thirdPerson;
extern vmCvar_t cg_thirdPersonRange;

extern vmCvar_t cg_drawGun;
extern vmCvar_t cg_gunX;
extern vmCvar_t cg_gunY;
extern vmCvar_t cg_gunZ;
extern vmCvar_t cg_gunScale;
extern vmCvar_t cg_gunWorldScale;
extern vmCvar_t cg_gunHoldForward;
extern vmCvar_t cg_gunHoldRight;
extern vmCvar_t cg_gunHoldUp;

/*
THE SLIDE, and the layout of the one animation the weapon model carries.

The clip is `fire`: five frames, slide at rest on 0, fully rearward on 2, back
at rest on 4. Three behaviours come out of it -- the cycle on each shot, the
middle frame held for an empty gun, and eventually the second half alone for
the slide releasing on a reload from empty.

CF_SLIDE_MS IS OURS, NOT THE ANIMATOR'S. The clip carries a framerate and we
ignore it, exactly as #art-spec promises: "animate at whatever pace looks
right -- we play it back time-scaled to our weapon table". A real Glock slide
cycles in something like 50ms; 70 reads better than it measures, because the
whole travel is over in four rendered frames at 60Hz and the eye needs a
moment to register that anything happened at all.

It must stay under the weapon's fire interval (100ms for the g17) or a second
shot arrives mid-cycle and restarts it from the top, which looks like the
slide stuttering rather than cycling.

IN THE HEADER because two files play it: cg_weapon.c for the gun in your own
hands and cg_ents.c for the one in everybody else's. Two copies of these
numbers would drift, and the symptom would be your slide and theirs cycling at
different speeds -- which nobody would ever notice and everybody would feel.
*/
#define CF_SLIDE_FIRST   0
#define CF_SLIDE_BACK    2
#define CF_SLIDE_LAST    4
#define CF_SLIDE_MS      70

/*
THE MAGAZINE, the weapon model's second clip: `reload`, straight after `fire`.
The magazine slides out of the grip, drops out of shot, stays gone, comes back
and seats. Generated by mapping/export_pistol.py (RELOAD_FRAMES there), and,
like the slide, played across the weapon table's reload time rather than at
its own rate -- see CG_WeaponReloadFrame.
*/
#define CF_MAG_FIRST     5
#define CF_MAG_COUNT     24

/*
THE GRIP: where a hand closes on the weapon model, in its own metres (+X
forward, +Z up). It is the magazine's centre, which sits inside the grip --
measured by mapping/export_pistol.py off the geometry and written to
mapping/g17_hold.json, and copied here from there. Both the hands you see in
first person and the ones other players hold the gun with put their Weapon
socket on this point.
*/
#define CF_GUN_GRIP_X   -0.0604f
#define CF_GUN_GRIP_Y    0.0f
#define CF_GUN_GRIP_Z    0.0670f

/*
THE MUZZLE: the middle of the barrel's front face, in the same metres, where
the flash is drawn. The model has no muzzle bone, so export_pistol.py measures
it off the barrel mesh into g17_hold.json, and it is copied here from there.
*/
#define CF_GUN_MUZZLE_X  0.0877f
#define CF_GUN_MUZZLE_Y  0.0f
#define CF_GUN_MUZZLE_Z  0.1452f

// What player body models are authored at: mapping/gen_body.py exports them
// at this many world units per metre (UNITS_PER_METRE there).
#define CF_BODY_UNITS_PER_METRE  31.0f

extern vmCvar_t cg_subtitles;
extern vmCvar_t cg_drawScoreboard;
extern vmCvar_t cg_drawStatus;
extern vmCvar_t cg_debugEvents;

//
// cg_main.c
//
void QDECL CG_Printf( const char *msg, ... ) Q_PRINTF_FUNC( 1, 2 );
void QDECL CG_Error( const char *msg, ... ) Q_NO_RETURN Q_PRINTF_FUNC( 1, 2 );
const char *CG_ConfigString( int index );
void CG_UpdateCvars( void );
void CG_ParseMatchState( void );
void CG_ParseScores( void );
void CG_NewClientInfo( int clientNum );
const char *CG_TeamName( team_t team );
const float *CG_TeamColor( team_t team );

//
// cg_snapshot.c
//
void CG_ProcessSnapshots( void );

//
// cg_predict.c
//
void CG_PredictPlayerState( void );
void CG_Trace( trace_t *result, const vec3_t start, const vec3_t mins, const vec3_t maxs,
               const vec3_t end, int skipNumber, int mask );
int  CG_PointContents( const vec3_t point, int passEntityNum );

//
// cg_view.c
//
void CG_DrawActiveFrame( int serverTime, stereoFrame_t stereoView, qboolean demoPlayback );

//
// cg_voice.c -- what the companion is told about the match
//
void CG_Voice_Frame( void );

//
// cg_ents.c
//
void CG_AddPacketEntities( void );
void CG_AddOwnBody( void );

//
// cg_players.c -- body model, animation, sockets
//
void     CG_RegisterBody( const char *name );
void     CG_ClearBodies( void );
qboolean CG_AddPlayerBody( centity_t *cent, const float *color, refEntity_t *hands );
qboolean CG_BodySocket( const refEntity_t *ent, const char *name, orientation_t *out );
float    CG_ReloadProgress( const centity_t *cent );
qboolean CG_AddViewArms( int clientNum, const refEntity_t *gun, float reload, const float *color );
void     CG_FlinchFromImpact( int shooter, const vec3_t impact );

//
// cg_weapon.c
//
void CG_AddViewWeapon( void );
void CG_WeaponReloadFrame( float progress, refEntity_t *gun );

//
// cg_draw.c
//
void CG_Draw2D( void );
void CG_DrawInformation( void );
void CG_FillRect( float x, float y, float width, float height, const float *color );
void CG_AdjustFrom640( float *x, float *y, float *w, float *h );
void CG_DrawString( float x, float y, const char *s, float charWidth, float charHeight,
                    const float *color );
void CG_DrawStringCentred( float cx, float y, const char *s, float charWidth, float charHeight,
                           const float *color );

//
// cg_scoreboard.c
//
qboolean CG_DrawScoreboard( void );
void     CG_DrawRoundStatus( void );
void     CG_UpdatePostgame( void );
void     CG_DrawPostgamePrompt( void );
void     CG_PostgameKey( int key );
void     CG_PostgameEventHandling( int event );

//
// cg_event.c
//
void CG_EntityEvent( centity_t *cent, const vec3_t position );
void CG_CheckEvents( centity_t *cent );
void CG_CheckPlayerstateEvents( playerState_t *ps, playerState_t *ops );
void CG_AddMuzzleFlash( const centity_t *cent, const refEntity_t *gun );

//
// cg_servercmds.c
//
void CG_ExecuteNewServerCommands( int latestSequence );

//
// cg_syscalls.c
//
void        trap_Print( const char *fmt );
void        trap_Error( const char *fmt ) Q_NO_RETURN;
int         trap_Milliseconds( void );
void        trap_Cvar_Register( vmCvar_t *vmCvar, const char *varName, const char *defaultValue, int flags );
void        trap_Cvar_Update( vmCvar_t *vmCvar );
void        trap_Cvar_Set( const char *var_name, const char *value );
void        trap_Cvar_VariableStringBuffer( const char *var_name, char *buffer, int bufsize );
int         trap_Argc( void );
void        trap_Argv( int n, char *buffer, int bufferLength );
void        trap_Args( char *buffer, int bufferLength );
void        trap_SendConsoleCommand( const char *text );
void        trap_VoiceState( const char *situation );
void        trap_VoiceNote( const char *event );
void        trap_VoiceEvent( const char *kind, const char *detail );
int         trap_VoiceSubtitle( char *buffer, int bufferSize );
void        trap_AddCommand( const char *cmdName );
void        trap_RemoveCommand( const char *cmdName );
void        trap_SendClientCommand( const char *s );
void        trap_UpdateScreen( void );
void        trap_CM_LoadMap( const char *mapname );
int         trap_CM_PointContents( const vec3_t p, clipHandle_t model );
void        trap_CM_BoxTrace( trace_t *results, const vec3_t start, const vec3_t end,
                              const vec3_t mins, const vec3_t maxs, clipHandle_t model, int brushmask );
clipHandle_t trap_CM_InlineModel( int index );
int         trap_CM_NumInlineModels( void );
void        trap_S_ClearLoopingSounds( qboolean killall );
void        trap_S_Respatialize( int entityNum, const vec3_t origin, vec3_t axis[3], int inwater );
sfxHandle_t trap_S_RegisterSound( const char *sample, qboolean compressed );
void        trap_S_StartLocalSound( sfxHandle_t sfx, int channelNum );
void        trap_S_StartSound( const vec3_t origin, int entityNum, int entchannel, sfxHandle_t sfx );
void        trap_R_LoadWorldMap( const char *mapname );
qhandle_t   trap_R_RegisterModel( const char *name );
qhandle_t   trap_R_RegisterShader( const char *name );
qhandle_t   trap_R_RegisterShaderNoMip( const char *name );
void        trap_R_ClearScene( void );
void        trap_R_AddRefEntityToScene( const refEntity_t *re );
void        trap_R_AddLightToScene( const vec3_t org, float intensity, float r, float g, float b );
int         trap_R_LerpTag( orientation_t *tag, qhandle_t mod, int startFrame, int endFrame,
                            float frac, const char *tagName );
qhandle_t   trap_R_RegisterSkin( const char *name );
int         trap_FS_FOpenFile( const char *qpath, fileHandle_t *f, fsMode_t mode );
void        trap_FS_Read( void *buffer, int len, fileHandle_t f );
void        trap_FS_FCloseFile( fileHandle_t f );
void        trap_R_RenderScene( const refdef_t *fd );
void        trap_R_SetColor( const float *rgba );
void        trap_R_DrawStretchPic( float x, float y, float w, float h,
                                   float s1, float t1, float s2, float t2, qhandle_t hShader );
void        trap_GetGlconfig( glconfig_t *glconfig );
void        trap_GetGameState( gameState_t *gamestate );
void        trap_GetCurrentSnapshotNumber( int *snapshotNumber, int *serverTime );
qboolean    trap_GetSnapshot( int snapshotNumber, snapshot_t *snapshot );
qboolean    trap_GetServerCommand( int serverCommandNumber );
int         trap_GetCurrentCmdNumber( void );
qboolean    trap_GetUserCmd( int cmdNumber, usercmd_t *ucmd );
void        trap_SetUserCmdValue( int stateValue, float sensitivityScale );
int         trap_Key_IsDown( int keynum );
int         trap_Key_GetCatcher( void );
void        trap_Key_SetCatcher( int catcher );
int         trap_RealTime( qtime_t *qtime );

#endif // CF_CG_LOCAL_H



