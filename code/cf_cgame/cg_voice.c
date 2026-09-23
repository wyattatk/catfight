/*
===========================================================================
catfight -- what the companion is told about the match.

THE ENGINE DOES NOT KNOW WHAT A STAT MEANS, and this file is why. cl_voice.c
speaks to the helper in terms of opaque text; deciding that stats[7] is her
health is game knowledge, and game knowledge lives in the game modules.

That separation was not a preference, it was forced and then turned out to be
right. The first attempt read cl.snap.ps directly from the engine, which meant
including cf_shared.h there -- and cf_shared.h is catfight's replacement for
id's bg_public.h, which the engine still includes. Both define statIndex_t,
STAT_HEALTH and PERS_SCORE with different meanings. They cannot coexist, and
the compiler saying so was the design telling us where the line was.

So cgame builds the sentences and hands them straight over:

    trap_VoiceState( situation )      whenever it has meaningfully changed
    trap_VoiceNote( event )           something she might remark on
    trap_VoiceEvent( kind, detail )   something that changes what she is to him

Dedicated syscalls rather than console commands. The console route silently
did not work from inside the render -- cgame reported sending and a CG_Printf
beside it appeared in the log, while the queued command never executed. It also
needed hex encoding purely to survive tokenisation, which is now gone.

WHETHER she acts on a note is the engine's business -- it owns the cooldown.
WHAT happened is ours.

A NOTE AND AN EVENT ARE NOT THE SAME THING and the difference is worth
holding on to. A note is prose, it is a suggestion, and dropping one costs
nothing -- she is under a cooldown and most of them are meant to go unsaid.
An event is a named fact that something on the other side of the pipe does
arithmetic with: how many matches they have played, whether he is on a losing
run, how she feels about tonight. It is never gated and never dropped.

The vocabulary of kinds is small and fixed for the same reason the cf_cmd
vocabulary is. Prose is a bad API for a counter, and a companion whose sense
of the relationship is parsed out of English sentences is one string change
away from forgetting a hundred matches.
===========================================================================
*/

#include "cg_local.h"

// She only reads the situation when asked to speak, so a second of staleness
// costs nothing. Health ticks every frame in a firefight; sending that would
// be sixty updates a second of something nobody is reading.
#define CG_VOICE_INTERVAL	1000
// Repeat even when nothing changed. See the note in CG_Voice_Frame -- a
// change-only feed loses its very first send to the map-load buffer flush.
#define CG_VOICE_RESEND		5000

static const char *cg_voiceMove[] = {
	"following him", "holding position", "pushing", "falling back", "moving to a point"
};
static const char *cg_voiceEngage[] = {
	"weapons free", "returning fire only", "holding fire"
};
static const char *cg_voiceFocus[] = {
	"picking her own targets", "on his target", "going for the human", "going for their companion"
};

/*
The situation is now the score, who is alive, both their hit rates and the
state of the match, so it no longer fits the sentence-sized buffer it started
in. It must match MAX_VOICE_SITUATION on the engine side -- the engine hex
encodes what it is handed and silently truncates past its own limit, and a
situation cut off mid-number is worse than one that was never sent.
*/
/*
2048 was sized before the situation carried a round-by-round history. Measured
at 1258 bytes without one; sixteen rounds of history adds about 700, which lands
close enough to the old cap that a long match would have started truncating --
and a truncated situation is the failure this file has spent the most time on.
Raised with room rather than to fit, because the next thing added to the feed
should not have to think about this again.

MUST MATCH MAX_VOICE_SITUATION in code/client/cl_voice.c. The engine hex encodes
what it is handed and sizes its own buffers from that constant.
*/
#define CG_VOICE_SITUATION	4096

/*
Rounds of history kept for the situation. Comfortably over the longest match the
pacing allows (cf_maxRounds is 7), so a whole match fits and the cap is a guard
against a server configured past that rather than a window that slides.
*/
#define CG_VOICE_MAX_ROUNDS	16

typedef struct {
	int			lastSentAt;
	int			lastFullSendAt;
	char		lastState[CG_VOICE_SITUATION];

	qboolean	haveSeen;
	int			seenOrders;
	int			seenAllyHealth;
	int			seenEliminated;
	int			seenScore;
	int			seenDeaths;

	/*
	How many of them were standing, and how many are now.

	`enemiesAlive` is written by the situation builder, which already counts it
	for the "Still alive" line; `seenEnemies` is what the edge below compares
	against.

	AN ENEMY DYING WAS THE ONE EDGE MISSING, and its absence read as her being
	wrong about the round rather than uninformed about it. She was told the
	count -- the situation carries it and is resent whenever the text changes --
	but nothing ever pointed AT the change, and every other thing worth noticing
	had an edge: he won a round, he died, you died, he is out, you are stuck.
	So after he shot somebody she kept talking from the conversation, where
	nobody had died yet, while the correct number sat in front of her unread.
	Reported from play as her being sure nobody had died, just after a kill.
	*/
	int			enemiesAlive;
	int			seenEnemies;

	/*
	His counters as they stood when THIS ROUND began, so the round can be
	reported as a difference.

	Every number she had was a match total or a match history, and all of them
	were labelled carefully as such -- which stopped her asserting a wrong one
	and did not give her the right one. The question he actually asks between
	rounds is "how did I do that round", and she had nothing whatsoever to
	answer it with. Correctly saying "two kills this MATCH" to a man asking
	about a round is an answer he will hear as being about the round.

	Kept as a baseline rather than counted, because every counter here is
	cumulative and replicated already: the round is the difference, needs no new
	field in the playerState, and cannot drift out of step with the total.
	*/
	int			roundBaseKills;
	int			roundBaseDeaths;
	int			roundBaseShots;
	int			roundBaseHits;
	int			roundBaseNumber;

	// The match, for the events. Tracked separately from the note edges above
	// because these must not be missed: a note that goes unsaid costs nothing,
	// a match that goes uncounted is a hundred matches together that never
	// happened.
	qboolean	inMatch;
	int			seenRounds[TEAM_NUM_TEAMS];

	/*
	WHAT HAS ALREADY HAPPENED IN THIS MATCH, which she had no way to know.

	The situation is a snapshot of NOW. Her only other record is the one-line
	episode written after the match is over. So between rounds -- which is when
	he actually talks to her, because orders go through keybinds and the talking
	is the strategy -- she could not discuss round two. Asked why they keep
	losing, she had the current score and nothing that happened to produce it,
	and a companion reasoning about a match she cannot remember is guessing in
	the register of someone who knows.

	Deliberately the OUTCOME of each round and not a narrative: who took it,
	whether he was standing at the end, whether she was. Three bits is enough
	for "you have lost three straight and gone down first in all of them", which
	is the shape of every useful thing either of them can say about a pattern.
	Anything richer is a story, and a story is what the episode summary is for.
	*/
	int			roundsPlayed;
	struct {
		qboolean	won;
		qboolean	heSurvived;
		qboolean	sheSurvived;
	}			round[CG_VOICE_MAX_ROUNDS];
} cgVoice_t;

static cgVoice_t	cgv;



/*
================
CG_Voice_MapName

cgs.mapname is "maps/cf_test.bsp", which is a path and not the name of
anywhere. Left as-is she says they are playing on maps/cf_test.bsp, which is
the single most obvious way for a person to stop sounding like one.

Caught by running the game rather than by reading the code, which is the
argument VOICE.md already makes for doing that early.
================
*/
static const char *CG_Voice_MapName( void ) {
	static char	name[MAX_QPATH];

	COM_StripExtension( COM_SkipPath( cgs.mapname ), name, sizeof( name ) );
	return name;
}

/*
================
CG_Voice_Format

The shape of the match: "Best of 3, first to 2."

Both halves or neither. The cap alone ("best of 3") does not say what winning
takes, and the limit alone ("first to 2") does not say how much match is left,
and she needs to be able to answer both questions. Either may be zero -- an
uncapped match, or a server too old to send the field -- and a number that was
never sent must not be spoken as though it were, so each is omitted separately.

Returns an empty string when nothing is known, which the callers append
harmlessly.
================
*/
static const char *CG_Voice_Format( void ) {
	static char	s[64];

	if ( cgs.maxRounds > 0 && cgs.roundLimit > 0 ) {
		Com_sprintf( s, sizeof( s ), " Best of %i, first to %i.",
		             cgs.maxRounds, cgs.roundLimit );
	} else if ( cgs.maxRounds > 0 ) {
		Com_sprintf( s, sizeof( s ), " Best of %i.", cgs.maxRounds );
	} else if ( cgs.roundLimit > 0 ) {
		Com_sprintf( s, sizeof( s ), " First to %i.", cgs.roundLimit );
	} else {
		s[0] = '\0';
	}

	return s;
}

/*
================
CG_Voice_Round

"Round 2 of 3", or "Round 2" when the server did not say how many there are.

A bare round number is very nearly useless to her: "round 2" is the start of a
match or the end of one depending on a number she did not have until the cap
started coming down the wire with the score.
================
*/
static const char *CG_Voice_Round( void ) {
	static char	s[32];

	if ( cgs.maxRounds > 0 ) {
		Com_sprintf( s, sizeof( s ), "Round %i of %i", cgs.roundNumber, cgs.maxRounds );
	} else {
		Com_sprintf( s, sizeof( s ), "Round %i", cgs.roundNumber );
	}

	return s;
}

/*
================
CG_Voice_LiveDanger

The danger line for a live round, which is the only state where it depends on
anything other than the phase.

WHO IS ALREADY DEAD CHANGES WHAT DANGER MEANS, and getting that wrong was the
loudest thing she did. A round stays MS_LIVE after somebody is eliminated, so
"DANGER: YES, talk like someone who might get shot mid-sentence" kept being
said to a player who was watching his own corpse from a free camera -- and to
her while she was out of it herself. She cannot tell the two of them apart if
the one line she is told to trust says the same thing either way.

So each case names WHO is dead in the same words the lines below use, and says
what that person can still do. "He is out" is not enough on its own; a model
asked to act on it will still reach for urgency unless it is told, plainly,
that there is nothing left to be urgent about.

A player with no companion in the round falls into the first case with him: he
is the only person who can be hurt, which is exactly what the original line
says.
================
*/
static const char *CG_Voice_LiveDanger( qboolean heIsUp, qboolean hasAlly,
                                        qboolean sheIsUp ) {
	if ( heIsUp && ( sheIsUp || !hasAlly ) ) {
		return "DANGER: YES. A live round is the ONE time either of you is "
		       "really in danger. Dying puts you out of the round and can lose "
		       "it. Talk like someone who might get shot mid-sentence.\n";
	}

	if ( heIsUp ) {
		return "DANGER: to him, not to you. YOU ARE ALREADY DEAD and out of "
		       "this round. You cannot shoot, cannot be shot, cannot move "
		       "anywhere and cannot carry out an order until the next round "
		       "starts. He is still alive and on his own, and watching him is "
		       "the only thing left that you can do.\n";
	}

	if ( hasAlly && sheIsUp ) {
		return "DANGER: to you, not to him. HE IS ALREADY DEAD and out of this "
		       "round -- watching from a camera, unable to shoot, unable to be "
		       "shot, unable to help. Do not ask him to do anything and do not "
		       "warn him about anything. YOU are the one still in this round "
		       "and it is still winnable.\n";
	}

	return "DANGER: none to him any more. HE IS ALREADY DEAD and out of this "
	       "round, and you are not standing in it either. Nothing either of "
	       "you does now changes how it ends.\n";
}

/*
================
CG_Voice_Phase

Where they are in the match, and WHETHER ANYONE CAN BE HURT. Two lines, always
both, in every state.

THE DANGER LINE IS THE POINT, and it is stated explicitly rather than left to
be inferred from the phase, because inferring it is exactly what she was
getting wrong. Told only "warmup", a model reasons its way to a firefight --
the map is loaded, there are enemies on it, he has a gun -- and she would open
with tactical urgency while the two of them stood around waiting for a match to
start. The same in reverse at the home screen.

The honest rule, matching G_CanDamage:

  LIVE       real danger, and the only state with stakes attached
  WARMUP     shots DO land, but death is free and nothing counts, and her own
             trigger is locked by the ceasefire -- so no stakes
  everything else   nobody can be hurt at all; G_CanDamage returns false

Warmup is deliberately not flattened into "safe". Telling her nothing can
happen there would make her wrong the moment he shot her in the face for fun,
which is a thing people do in warmup and the reason warmup allows it.

Written as a labelled line rather than as prose alone. The rest of this file
argues for prose and is right to -- prose carries meaning that a field name
cannot -- but a binary that she must never misread wants a stable anchor she
can find in the same place every time, so it gets a label AND the prose.

The live case is the one that needs to know who is still standing; see
CG_Voice_LiveDanger. Every other state's answer is the same for everybody in
it, which is why only that one takes the arguments.
================
*/
static void CG_Voice_Phase( char *out, int size, qboolean heIsUp,
                            qboolean hasAlly, qboolean sheIsUp ) {
	switch ( cgs.matchState ) {
		case MS_WARMUP:
			Q_strcat( out, size, va( "Warmup on %s. THE MATCH HAS NOT STARTED.\n",
			                         CG_Voice_MapName() ) );
			Q_strcat( out, size,
				"DANGER: none. Shots do land in warmup, but dying just respawns "
				"you and nothing counts. You cannot shoot back yet -- your "
				"trigger stays locked until the match starts. This is standing "
				"around, not fighting.\n" );
			break;

		case MS_COUNTDOWN:
			Q_strcat( out, size, va( "%s on %s is about to start.%s\n",
			                         CG_Voice_Round(), CG_Voice_MapName(),
			                         CG_Voice_Format() ) );
			Q_strcat( out, size,
				"DANGER: not yet. Nobody can be hurt until the round goes live, "
				"which is any second now.\n" );
			break;

		case MS_ROUND_END:
			Q_strcat( out, size, va( "%s just ended. Short break before the next.%s\n",
			                         CG_Voice_Round(), CG_Voice_Format() ) );
			Q_strcat( out, size,
				"DANGER: none. The shooting is over until the next round starts.\n" );
			break;

		case MS_MATCH_END:
			Q_strcat( out, size, "The match is over.\n" );
			Q_strcat( out, size,
				"DANGER: none. It is finished. Nobody can be hurt.\n" );
			break;

		case MS_PAUSED:
			Q_strcat( out, size, "The match is paused -- somebody left and it is "
			                     "being held for them.\n" );
			Q_strcat( out, size,
				"DANGER: none while it is paused. Nobody can be hurt.\n" );
			break;

		default:
			Q_strcat( out, size, va( "%s on %s, LIVE.%s\n",
			                         CG_Voice_Round(), CG_Voice_MapName(),
			                         CG_Voice_Format() ) );
			Q_strcat( out, size, CG_Voice_LiveDanger( heIsUp, hasAlly, sheIsUp ) );
			break;
	}
}

/*
================
CG_Voice_HitRate

What a hit rate is WORTH, which is the half of it she could not work out.

The number on its own was actively harmful. Told "he is hitting 67%", a model
reaches for whatever accuracy meant in whatever it learned it from -- and two
thirds sounds like a fail, so she nagged a player who was shooting well, over
and over, because the figure was the only hard number in front of her and it
looked bad. Missing one shot in three with this gun is not missing badly.

What counts as good is a fact about THIS game's weapon: one semi-automatic
pistol, one bullet per trigger pull, no pellets, four hits to kill at close
range and six at distance, fired by somebody strafing. That is game knowledge,
so it belongs on this side of the pipe with the rest of it -- the same argument
the top of this file makes about health and the score.

The bands are deliberately generous, because the two mistakes do not cost the
same. Calling a good player average makes her a bore for one line. Calling an
average player bad, every time she speaks, is the thing that made him stop
wanting to hear from her.
================
*/
static const char *CG_Voice_HitRate( int pct ) {
	if ( pct >= 50 ) {
		return "That is exceptional shooting.";
	}
	if ( pct >= 35 ) {
		return "That is good shooting, and NOT something to pick at.";
	}
	if ( pct >= 22 ) {
		return "That is an ordinary, healthy hit rate and is not worth a remark.";
	}
	if ( pct >= 14 ) {
		return "That is on the low side.";
	}
	return "That is genuinely bad, and this is the one band worth saying so about.";
}

/*
================
CG_Voice_Situation

Everything she knows about the match. Written as prose rather than as fields
because it is going into a prompt, and a model reads "You are down" more
reliably than it reads allyHealth=0 -- and reads "one round from winning" far
more reliably than it works that out from two integers and a limit.
================
*/
static void CG_Voice_Situation( char *out, int size ) {
	const playerState_t	*ps = &cg.snap->ps;
	int					orders = ps->stats[STAT_ALLY_ORDERS];
	team_t				mine = (team_t)ps->persistant[PERS_TEAM];
	team_t				theirs = ( mine == TEAM_RED ) ? TEAM_BLUE : TEAM_RED;
	int					us, them, ourSide, theirSide, i;
	int					hisAcc, herAcc;
	qboolean			hasAlly, heIsUp, sheIsUp, sheIsOut;

	/*
	WHO IS ON THEIR FEET, worked out once and then said the same way everywhere
	below. She was getting this wrong in both directions, and the reason is that
	neither answer was a field she could read.

	His: eliminated AND health, not either alone. ClientBecomeSpectator
	deliberately refills a dead player's health so his own HUD is not showing a
	corpse's bar -- which meant the old line here announced "100 of 100 health"
	about a man who had been dead for ten seconds, with nothing but a trailing
	clause to say otherwise. Health alone is wrong for the same reason in
	reverse during the beat between the shot landing and the camera coming
	free, when he is dead and the flag has not caught up, and in warmup, where
	being on nought health is a moment rather than a state.

	Hers arrives already correct -- G_UpdateAllyStatus zeroes an eliminated
	teammate's health precisely so this does not have to be guessed at -- but it
	means nothing without knowing whether she is in the round at all, so the two
	are kept apart.
	*/
	hasAlly = (qboolean)( ps->stats[STAT_ALLY_CLIENT] != 0 );
	heIsUp  = (qboolean)( !ps->persistant[PERS_ELIMINATED]
	                      && ps->stats[STAT_HEALTH] > 0 );
	sheIsUp = (qboolean)( hasAlly && ps->stats[STAT_ALLY_HEALTH] > 0 );

	/*
	AND WHETHER SHE IS ACTUALLY OUT, which is not the same question as whether
	her health is zero -- and treating it as the same is what had her announcing
	her own death while standing up.

	The note above says hers "arrives already correct" because G_UpdateAllyStatus
	zeroes an eliminated teammate's health. That is true and it is not enough:
	zero health ALSO means she is on the floor in warmup waiting to respawn, and
	it means the beat after a round restart before her health has been set. In
	both she is told "YOU ARE DEAD, OUT of this round, do not agree to do
	anything" -- in warmup, where there is no round to be out of and she is back
	up in a moment.

	His state was given three cases for exactly this reason and hers was left
	with two. The flag is authoritative and already replicated in the
	scoreboard's configstring, which is the same source the enemy count reads,
	so ask it rather than inferring from a number that means three things.
	*/
	sheIsOut = qfalse;
	if ( hasAlly ) {
		int allyNum = ps->stats[STAT_ALLY_CLIENT] - 1;

		if ( allyNum >= 0 && allyNum < MAX_CLIENTS
		     && cgs.clientinfo[allyNum].infoValid ) {
			sheIsOut = cgs.clientinfo[allyNum].eliminated;
		}
	}

	Com_sprintf( out, size, "=== RIGHT NOW ===\n" );

	/*
	The state of the match and whether anyone can be hurt, and it is first
	because everything below it means something different depending on these two
	lines. Ninety health is comfortable in warmup and precarious at match point.
	*/
	CG_Voice_Phase( out, size, heIsUp, hasAlly, sheIsUp );

	/*
	THE SCORE, which she could not see at all until now.

	The old block reported PERS_SCORE as "rounds won", which is not the score:
	it counts rounds this player was ALIVE at the end of, so a round his team
	won while he was dead did not appear. A companion who does not know whether
	they are winning cannot say a single useful thing about the match, and
	worse, she confidently said the wrong number.

	Written with the stakes attached rather than as two integers, because "4-1,
	one round from taking it" is a thing to have an opinion about and "4 1" is
	not.
	*/
	if ( cgs.matchState != MS_WARMUP && ( mine == TEAM_RED || mine == TEAM_BLUE ) ) {
		int	ours = cgs.teamRounds[mine];
		int	hers = cgs.teamRounds[theirs];

		Q_strcat( out, size, va( "Score: %i-%i%s.", ours, hers,
		                         ours > hers ? " up" : ( ours < hers ? " down" : ", level" ) ) );

		/*
		Decided first, THEN match point. The obvious version tests
		>= limit - 1, which is still true at the limit itself -- so a match
		that had just been lost 0-4 came out as "one round from LOSING the
		match", which is the confident kind of wrong that makes her sound like
		she is not watching. Caught by playing a match rather than by reading
		this, which is becoming a theme.
		*/
		if ( cgs.roundLimit > 0 ) {
			if ( ours >= cgs.roundLimit ) {
				Q_strcat( out, size, " You took the match." );
			} else if ( hers >= cgs.roundLimit ) {
				Q_strcat( out, size, " They took the match." );
			} else if ( ours == cgs.roundLimit - 1 && ours > hers ) {
				Q_strcat( out, size, " ONE ROUND from winning the match." );
			} else if ( hers == cgs.roundLimit - 1 && hers > ours ) {
				Q_strcat( out, size, " One round from LOSING the match." );
			}
			/*
			No "first to N" fallback here any more -- the phase line above now
			states the whole format, and saying it twice in one situation is how
			she ends up repeating the rules at him instead of playing.
			*/
		}

		/*
		The decider, which is a different feeling from any other round and was
		invisible before the cap came down the wire. At best-of-3 and one round
		each, round 3 IS the match, and that is worth her knowing in those words
		rather than as two numbers that happen to add up.
		*/
		if ( cgs.maxRounds > 0 && cgs.roundNumber >= cgs.maxRounds
		     && ( cgs.matchState == MS_LIVE || cgs.matchState == MS_COUNTDOWN )
		     && ( cgs.roundLimit <= 0
		          || ( ours < cgs.roundLimit && hers < cgs.roundLimit ) ) ) {
			Q_strcat( out, size, ours == hers
				? " LAST ROUND and you are level -- this one decides the match."
				: " LAST ROUND of the match. There is no next one." );
		}

		Q_strcat( out, size, "\n" );
	}

	/*
	Who is still standing. Also new, and the other thing she could not see.

	Read off the scoreboard rather than off entities on purpose: an entity only
	reaches this client while it is in the PVS, so counting live enemies from
	what is being rendered would tell her everyone is dead the moment the fight
	moves into the next room. Same argument as STAT_ALLY_HEALTH.
	*/
	us = them = ourSide = theirSide = 0;
	for ( i = 0; i < MAX_CLIENTS; i++ ) {
		const clientInfo_t *ci = &cgs.clientinfo[i];

		if ( !ci->infoValid ) {
			continue;
		}
		if ( ci->team == mine ) {
			ourSide++;
			if ( !ci->eliminated ) {
				us++;
			}
		} else if ( ci->team == theirs ) {
			theirSide++;
			if ( !ci->eliminated ) {
				them++;
			}
		}
	}

	/*
	HOW MANY THERE EVER WERE, which is the number that makes every other number
	here mean something, and the one she was missing entirely.

	Told "he got 2 kills", a model with no idea how big a side is decides two is
	a thin round and says so -- when two is the whole enemy team and the most
	anybody could possibly have got. She was marking him against a scoreboard
	from some other game. A ceiling is game knowledge and is stated outright for
	the same reason the danger line is, rather than left to be inferred from a
	roster she cannot see.
	*/
	if ( theirSide > 0 && ( mine == TEAM_RED || mine == TEAM_BLUE ) ) {
		/*
		Spelled singular or plural rather than left as "1 enemies". She does not
		read this out, she reads it and then talks -- but a sentence that was
		ungrammatical going in comes back sounding like a readout rather than
		like her, and the whole point of writing the feed as prose is that it
		does not.
		*/
		Q_strcat( out, size, va(
			"Sides: %i on yours counting you and him, %i on theirs. THERE %s "
			"ONLY %i %s IN THE WHOLE ROUND, so %i %s in one round is every "
			"enemy there was -- the maximum, not a modest haul. A round is over "
			"the moment one side is down, so most of them are decided by one or "
			"two kills in total.\n",
			ourSide, theirSide,
			theirSide == 1 ? "IS" : "ARE", theirSide,
			theirSide == 1 ? "ENEMY" : "ENEMIES",
			theirSide, theirSide == 1 ? "kill" : "kills" ) );
	}

	/*
	HOW THEY GOT TO THIS SCORE, round by round.

	The score says 1-3 and says nothing about why, and "why" is the entire
	content of talking tactics between rounds. Written as a list rather than
	prose so she can count it -- a model asked to spot a pattern does far better
	against four short lines than against a sentence that has already decided
	what the pattern was.

	Each line carries who took the round and who was left standing, because the
	interesting patterns are about order of death rather than about the result:
	going down first every time and winning anyway is a completely different
	conversation from trading evenly and losing on the last round.
	*/
	if ( cgv.roundsPlayed > 0 ) {
		Q_strcat( out, size, "HOW THIS MATCH HAS GONE, round by round:\n" );
		for ( i = 0; i < cgv.roundsPlayed; i++ ) {
			Q_strcat( out, size, va( "  Round %i: %s. %s, %s.\n",
				i + 1,
				cgv.round[i].won ? "you took it" : "they took it",
				cgv.round[i].heSurvived  ? "he lived"  : "he died",
				cgv.round[i].sheSurvived ? "you lived" : "you died" ) );
		}
	}

	// Recorded for the edge in CG_Voice_Frame, which is the only other thing
	// that needs this count and should not walk the roster a second time.
	cgv.enemiesAlive = them;

	if ( cgs.matchState == MS_LIVE && ( us || them ) ) {
		Q_strcat( out, size, va( "Still alive: %i of you against %i of them.%s\n",
			us, them,
			us > them ? " You have the advantage RIGHT NOW -- use it."
			          : ( us < them ? " You are outnumbered." : "" ) ) );
	}

	/*
	HIM, and whether he is standing comes first rather than last.

	The old line led with a health figure and hung "He is OUT of this round" off
	the end of it, which is the wrong way round twice over: the trailing clause
	is the important half, and the health figure in front of it is a spectator's
	refilled bar rather than anything about a living man. She read the number,
	believed it, and talked to a dead player.
	*/
	if ( heIsUp ) {
		Q_strcat( out, size, va( "HE IS ALIVE: %i of %i health, %i loaded and %i spare.%s\n",
			ps->stats[STAT_HEALTH], ps->stats[STAT_MAX_HEALTH],
			ps->ammo[ps->weapon], ps->stats[STAT_RESERVE],
			// Only when he is actually holding something. WP_NONE indexes an
			// ammo slot that is always zero, and announcing an empty gun to
			// somebody who has no gun is the kind of confident wrongness she
			// never recovers from.
			( ps->weapon != WP_NONE && ps->ammo[ps->weapon] == 0 )
				? " HIS GUN IS EMPTY." : "" ) );
	} else if ( ps->persistant[PERS_ELIMINATED] ) {
		Q_strcat( out, size,
			"HE IS DEAD. He was killed and is OUT of this round, watching it "
			"from a camera. He cannot shoot, cannot be shot, cannot move and "
			"cannot act on anything you tell him until the next round starts. "
			"Whatever number you see for his health, he is not standing.\n" );
	} else {
		// Warmup, where being on the floor is a moment rather than a state.
		Q_strcat( out, size,
			"He is down, and back up in a moment -- this is warmup and it "
			"costs him nothing.\n" );
	}

	if ( !hasAlly ) {
		Q_strcat( out, size, "You are not in this round with him.\n" );
	} else if ( !sheIsUp && sheIsOut ) {
		Q_strcat( out, size,
			"YOU ARE DEAD. You were killed and are OUT of this round. You "
			"cannot shoot, cannot be shot, cannot go anywhere and cannot carry "
			"out a single order until the next round starts -- so do not agree "
			"to do anything, and do not describe yourself doing anything. "
			"Talking is all that is left.\n" );
	} else if ( !sheIsUp ) {
		// On the floor with no round to be out of: warmup, or the beat after a
		// restart before her health has been set. She is back up in a moment and
		// must not be told to stop acting. Mirrors his third case exactly.
		Q_strcat( out, size,
			"You are down for a moment and back up shortly -- nothing here "
			"counts and you are not out of anything.\n" );
	} else {
		Q_strcat( out, size, va( "YOU ARE ALIVE: %i of %i health, %s, %s, %s.%s\n",
			ps->stats[STAT_ALLY_HEALTH], ps->stats[STAT_ALLY_MAX_HEALTH],
			cg_voiceMove[ CF_ORDERS_MOVE( orders ) % ARRAY_LEN( cg_voiceMove ) ],
			cg_voiceEngage[ CF_ORDERS_ENGAGE( orders ) % ARRAY_LEN( cg_voiceEngage ) ],
			cg_voiceFocus[ CF_ORDERS_FOCUS( orders ) % ARRAY_LEN( cg_voiceFocus ) ],
			CF_ORDERS_STUCK( orders ) ? " You CANNOT REACH where he sent you." : "" ) );
	}

	/*
	THIS ROUND FIRST, because it is the one he is asking about.

	A difference against the baseline taken when the round began. Printed above
	the match totals and in the same shape, so the two are adjacent and the
	only thing separating them is the words THIS ROUND and WHOLE MATCH -- which
	is the comparison she needs to be able to make, and which she cannot make at
	all if only one of them exists.

	Shots rather than a percentage: a round is a handful of rounds fired, and a
	percentage off four shots is the sort of number that sounds authoritative
	and means nothing. CF_ACCURACY_MIN_SHOTS exists for exactly that reason on
	the match figure below.
	*/
	if ( cgs.matchState == MS_LIVE || cgs.matchState == MS_ROUND_END ) {
		Q_strcat( out, size, va(
			"THIS ROUND so far: %i kills, %i deaths, %i of %i shots landed.\n",
			ps->persistant[PERS_KILLS] - cgv.roundBaseKills,
			ps->persistant[PERS_DEATHS] - cgv.roundBaseDeaths,
			ps->persistant[PERS_HITS] - cgv.roundBaseHits,
			ps->persistant[PERS_SHOTS] - cgv.roundBaseShots ) );
	}

	/*
	Said as match totals in so many words. "Tally: 1 rounds he survived, 2
	kills, 2 deaths" reads as though it might be this round's, and a companion
	who mistakes a match total for a round total is doing arithmetic against the
	wrong denominator every time she opens her mouth.
	*/
	Q_strcat( out, size, va(
		"His totals for the WHOLE MATCH so far, not for this round: %i kills, "
		"%i deaths, still standing at the end of %i rounds.\n",
		ps->persistant[PERS_KILLS], ps->persistant[PERS_DEATHS],
		ps->persistant[PERS_SCORE] ) );

	/*
	How the two of them are actually shooting.

	Reported only once there are enough shots to mean anything -- one lucky
	round is not "100% accuracy", it is one round, and a companion who says
	the former sounds like a spreadsheet with a grudge. Below the threshold
	the line is simply absent, which is the honest way to say "not enough to
	go on" to something that will otherwise improvise a number.

	Hers arrives as the percentage plus one so that zero can mean "too few
	shots"; see STAT_ALLY_ACCURACY.
	*/
	hisAcc = ps->persistant[PERS_SHOTS] >= CF_ACCURACY_MIN_SHOTS
	         ? CF_Accuracy( ps->persistant[PERS_SHOTS], ps->persistant[PERS_HITS] )
	         : -1;
	herAcc = ps->stats[STAT_ALLY_ACCURACY] - 1;

	if ( hisAcc >= 0 ) {
		Q_strcat( out, size, va( "He is hitting %i%% of his shots this match (%i of %i). %s",
			hisAcc, ps->persistant[PERS_HITS], ps->persistant[PERS_SHOTS],
			CG_Voice_HitRate( hisAcc ) ) );
		if ( herAcc >= 0 ) {
			/*
			Fifteen points apart rather than ten. Both figures are drawn from a
			few dozen shots, where ten points is one good exchange either way --
			and "you are outshooting him" off the back of noise is a thing she
			would say and then have to live with for the rest of the match.

			"And he knows it" went with it. Nothing here knows what he knows,
			and handing her a jab pre-written is how a tease turns into the
			needling he already gets too much of.
			*/
			Q_strcat( out, size, va( " You are hitting %i%%.%s", herAcc,
				herAcc > hisAcc + 15 ? " You are outshooting him." :
				( hisAcc > herAcc + 15 ? " He is outshooting you." : "" ) ) );
		}
		Q_strcat( out, size, "\n" );
	} else if ( herAcc >= 0 ) {
		Q_strcat( out, size, va( "You are hitting %i%% of your shots this match. %s\n",
		                         herAcc, CG_Voice_HitRate( herAcc ) ) );
	}
}

/*
================
CG_Voice_Match

The events that change what she is to him, rather than what she might say.

Run every frame and not on the situation feed's one-second timer, which is the
one real difference in how the two are handled. A round can end and the next
begin inside a second, and a companion who "has played 200 matches with you"
has to have counted every one of them -- the moment the counter is only
approximately right is the moment the whole memory stops being believable.

Result is decided from the round tallies rather than from PERS_SCORE, because
PERS_SCORE counts rounds he was ALIVE at the end of. Winning a round he was
eliminated in does not move it, and reading a match result off it would tell
her he lost a match he had just won.
================
*/
static void CG_Voice_Match( void ) {
	const playerState_t	*ps = &cg.snap->ps;
	team_t				mine = (team_t)ps->persistant[PERS_TEAM];
	team_t				theirs = ( mine == TEAM_RED ) ? TEAM_BLUE : TEAM_RED;
	matchState_t		state = cgs.matchState;
	qboolean			playing;
	int					i;

	if ( mine != TEAM_RED && mine != TEAM_BLUE ) {
		// Spectating, or in warmup with no side. Nothing here is his.
		return;
	}

	// A round went to somebody. Reported from the tally, so it is right for
	// the rounds he did not survive as well as the ones he did.
	for ( i = TEAM_RED; i <= TEAM_BLUE; i++ ) {
		if ( cgs.teamRounds[i] > cgv.seenRounds[i] && cgv.inMatch ) {
			/*
			Recorded at the instant the tally moves, for the same reason the
			clutch test below reads STAT_ALLY_HEALTH here: this is the last
			moment both of their end-of-round states are still true. A frame
			later the next spawn has overwritten them, and the history would
			say everybody survived every round.
			*/
			if ( cgv.roundsPlayed < CG_VOICE_MAX_ROUNDS ) {
				int r = cgv.roundsPlayed;

				cgv.round[r].won         = (qboolean)( i == mine );
				cgv.round[r].heSurvived   = (qboolean)( !ps->persistant[PERS_ELIMINATED]
				                                        && ps->stats[STAT_HEALTH] > 0 );
				cgv.round[r].sheSurvived  = (qboolean)( ps->stats[STAT_ALLY_CLIENT]
				                                        && ps->stats[STAT_ALLY_HEALTH] > 0 );
				cgv.roundsPlayed++;
			}

			if ( i == mine ) {
				/*
				He won it, and she was already down when he did.

				This is the closest the game currently gets to "he saved me",
				and it is worth reporting separately because it is the single
				most charged thing that reliably happens in a round: she was
				out, it was his to lose, and he did not lose it. A real save
				-- killing whoever was about to kill her -- needs the server
				to attribute it and belongs in cf_game with the obituaries.

				Read at the moment the tally moves, so STAT_ALLY_HEALTH is
				still whatever it was when the round ended rather than what
				the next spawn will set it to.
				*/
				if ( ps->stats[STAT_ALLY_CLIENT] && ps->stats[STAT_ALLY_HEALTH] <= 0 ) {
					trap_VoiceEvent( "he-clutched", "" );
				} else {
					trap_VoiceEvent( "round-won", "" );
				}
			} else {
				trap_VoiceEvent( "round-lost", "" );
			}
		}
		cgv.seenRounds[i] = cgs.teamRounds[i];
	}

	playing = (qboolean)( state == MS_COUNTDOWN || state == MS_LIVE
						  || state == MS_ROUND_END );

	if ( playing && !cgv.inMatch ) {
		cgv.inMatch = qtrue;
		// A new match, so the round history starts empty. Cleared HERE rather
		// than at match end, because the two ways a match stops -- finished, and
		// abandoned back to warmup -- would otherwise need to agree about it,
		// and one of them forgetting would carry the last match's rounds into
		// the next one's situation.
		cgv.roundsPlayed = 0;
		trap_VoiceEvent( "match-start", CG_Voice_MapName() );
	} else if ( cgv.inMatch && state == MS_MATCH_END ) {
		// The detail is the score, and it is the game's rather than something
		// she reconstructs later. A remembered score she got wrong reads as
		// lying, which is worse than not remembering the match at all.
		cgv.inMatch = qfalse;
		trap_VoiceEvent( cgs.teamRounds[mine] > cgs.teamRounds[theirs]
						 ? "match-won" : "match-lost",
						 va( "%s, %i-%i", CG_Voice_MapName(),
							 cgs.teamRounds[mine], cgs.teamRounds[theirs] ) );
	} else if ( cgv.inMatch && state == MS_WARMUP ) {
		// Dropped back to warmup: somebody left and the match is not going to
		// finish. It never happened, and telling her it did would put a match
		// in her history with no result attached to it.
		cgv.inMatch = qfalse;
	}
}

/*
================
CG_Voice_Frame

Called once a frame. Sends the situation when it changes, and reports the edges
worth remarking on.

Only edges. A companion told "he is on 30 health" every frame has nothing to
react to; one told "he just got himself killed" does. The engine decides
whether she is allowed to say anything about it.
================
*/
void CG_Voice_Frame( void ) {
	char		now[CG_VOICE_SITUATION];
	const char	*event = NULL;
	const playerState_t	*ps;
	int			orders, allyHealth, eliminated, score, deaths;

	if ( !cg.snap ) {
		return;
	}

	// Before the timer, deliberately. The match events must not be sampled at
	// 1Hz -- see the note above CG_Voice_Match.
	CG_Voice_Match();

	if ( cg.time - cgv.lastSentAt < CG_VOICE_INTERVAL ) {
		return;
	}
	cgv.lastSentAt = cg.time;

	/*
	Re-baseline the round counters BEFORE the situation is built, because the
	situation is what reads them. Done here rather than beside the other edges
	below, which run after the block has already been written -- the first
	report of every round would have carried the previous round's figures, and
	been resent as correct a second later, so it would have looked like her
	briefly misremembering rather than like an ordering mistake.

	Keyed on the ROUND NUMBER, not on the match state. Watching for MS_LIVE
	would re-baseline on every pass through the live state -- a reconnect, a
	pause resuming, the frame after a spawn -- and wipe a round's figures
	halfway through it, leaving her reporting nought kills to a man who has
	just got two. The round number moves once per round and only forward.

	The first sample of a session baselines too, since roundBaseNumber starts at
	zero and no round is zero. That is what stops a client joining mid-match
	from attributing the whole match so far to the round it walked into.
	*/
	if ( cgs.roundNumber != cgv.roundBaseNumber ) {
		cgv.roundBaseNumber = cgs.roundNumber;
		cgv.roundBaseKills  = cg.snap->ps.persistant[PERS_KILLS];
		cgv.roundBaseDeaths = cg.snap->ps.persistant[PERS_DEATHS];
		cgv.roundBaseShots  = cg.snap->ps.persistant[PERS_SHOTS];
		cgv.roundBaseHits   = cg.snap->ps.persistant[PERS_HITS];
	}

	CG_Voice_Situation( now, sizeof( now ) );

	/*
	Sent when it changes, AND repeated on a slow timer when it has not.

	The repeat re-syncs her if the helper is ever restarted under a running
	game, which a change-only feed could never do -- and it means a single
	dropped update is corrected within five seconds instead of leaving her
	quietly wrong for the rest of the match. She improvises well enough that a
	stale situation looks exactly like a current one, so the feed has to be
	self-healing rather than merely correct.
	*/
	if ( strcmp( now, cgv.lastState )
		 || cg.time - cgv.lastFullSendAt >= CG_VOICE_RESEND ) {
		Q_strncpyz( cgv.lastState, now, sizeof( cgv.lastState ) );
		cgv.lastFullSendAt = cg.time;
		trap_VoiceState( now );
	}

	ps         = &cg.snap->ps;
	orders     = ps->stats[STAT_ALLY_ORDERS];
	allyHealth = ps->stats[STAT_ALLY_HEALTH];
	eliminated = ps->persistant[PERS_ELIMINATED];
	score      = ps->persistant[PERS_SCORE];
	deaths     = ps->persistant[PERS_DEATHS];

	if ( cgv.haveSeen ) {
		/*
		The same two edges as the note chain below, reported as facts as well
		as as things to say. Not folded into that chain, because it picks ONE
		thing to remark on and throws the rest away -- which is exactly right
		for talking and exactly wrong for counting.

		Sampled on the one-second timer rather than per frame, and that is
		safe here in a way it is not for a round: this is an elimination mode,
		so neither of them can die twice inside a second.
		*/
		if ( deaths > cgv.seenDeaths ) {
			trap_VoiceEvent( "he-died", "" );
		}
		if ( allyHealth <= 0 && cgv.seenAllyHealth > 0 ) {
			trap_VoiceEvent( "she-died", "" );
		}
		/*
		One of them went down. Only a DECREASE counts -- a round restart puts
		them all back up, and reporting that as news would have her announcing a
		resurrection every round.
		*/
		if ( cgv.enemiesAlive < cgv.seenEnemies ) {
			trap_VoiceEvent( "enemy-died", "" );
		}
	}

	if ( !cgv.haveSeen ) {
		// First look. Everything is a change from nothing and none of it is
		// news, so record and stay quiet.
		cgv.haveSeen = qtrue;
	} else if ( score > cgv.seenScore ) {
		event = "he just won a round";
	} else if ( deaths > cgv.seenDeaths ) {
		event = "he just got himself killed";
	} else if ( allyHealth <= 0 && cgv.seenAllyHealth > 0 ) {
		event = "you just went down and left him on his own";
	} else if ( eliminated && !cgv.seenEliminated ) {
		event = "he is out for the rest of this round";
	} else if ( cgv.enemiesAlive < cgv.seenEnemies ) {
		/*
		Below his own death and his round win, because if he traded he should
		hear about the trade rather than about the kill, and above being stuck,
		which can wait. The wording says how many are LEFT rather than that one
		died: "one of them is down" invites her to ask which, and the number
		remaining is the thing either of them can act on.
		*/
		event = ( cgv.enemiesAlive > 0 )
		      ? "one of them just went down and there are still more of them up"
		      : "that was the last of them -- the round is his";
	} else if ( CF_ORDERS_STUCK( orders ) && !CF_ORDERS_STUCK( cgv.seenOrders ) ) {
		event = "you cannot reach the spot he sent you to";
	}

	cgv.seenOrders     = orders;
	cgv.seenAllyHealth = allyHealth;
	cgv.seenEliminated = eliminated;
	cgv.seenScore      = score;
	cgv.seenDeaths     = deaths;
	cgv.seenEnemies    = cgv.enemiesAlive;

	if ( event ) {
		trap_VoiceNote( event );
	}
}
