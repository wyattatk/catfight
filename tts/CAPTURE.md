# Capturing a voice

How to record a person so that a VITS model fine-tuned on the result sounds
like them talking, rather than like them reading.

This exists because the failure it prevents is expensive and invisible until
the end: you can run a perfect two-hour session, hand the audio to a training
pipeline, and only then find out the data was unusable. Every rule below is
about something that cannot be fixed afterwards.

## Why a fine-tune and not a better stock model

Measured on the dev machine (see `code/cf_game/VOICE.md`): Kokoro int8 ran at
**1.5x real time**, Piper at **RTF 0.04**. The GPU is already holding a 9B
language model, so the synthesiser gets CPU, and her whole budget from his
words to her voice is **860-1220ms**. That rules out the expressive stock
models outright.

But the ereader sound is not caused by the architecture. **It is caused by the
training data** -- most open voices are trained on audiobook corpora, so they
learned to read aloud. VITS is fast because of how it is built, not because of
whose voice it holds. Fine-tune it on conversation and you keep RTF 0.04 and
replace the register. That combination is not available any other way.

## The one rule that matters most

**DO NOT RECORD THE DISCORD STREAM.** Not the call, not a Craig bot, not
OBS capturing desktop audio.

Discord applies noise suppression, automatic gain control and lossy
compression, in that order, before anyone else hears you. All three are good
for a call and fatal for training data: AGC pumps the level around so the model
learns inconsistent loudness, noise suppression eats breath and sibilance, and
the codec throws away the high end that carries a voice's identity.

**She records herself, locally, raw.** Audacity or OBS on her own machine,
writing an uncompressed WAV. What reaches you over the call is a monitor feed,
not the take.

## Her setup

- **Closed-back headphones.** Not speakers, not open-backs. If game audio
  reaches her microphone it is in the training data, and the model will learn
  to synthesise gunfire underneath her voice. This one is unrecoverable.
- **One microphone, one position, one gain setting, for the whole session.** A
  boom arm helps. Moving closer and further changes the timbre and the model
  averages it into mush.
- **48 kHz, 24-bit, mono, WAV.** Downsampling later is free; resolution you
  never captured is not.
- **Set the gain for the shouting, not the talking.** She will get loud. Levels
  that feel right for conversation will clip when a round goes badly, and
  clipping cannot be undone. Peaks around -6 dBFS, normal speech sitting lower
  than feels comfortable.
- **A quiet room.** No fan, no AC, no open window, no second person.
- **Do not enable any noise suppression, gate, compressor or "voice
  enhancement"** in the recording software. Raw.

## Should you record while playing a shooter together?

**Yes. That is the right instinct and it is the only way to get the register.**
Nobody can perform "out of breath and annoyed after losing a round" on cue in a
booth. You have to actually lose the round.

But it needs structure, or you get two hours of audio and twenty usable
minutes.

### Session A -- gameplay, for register

Play together. She talks. That is the whole plan.

- **You stay quiet, or you are on a separate track.** Overlapping speech is
  unusable and there is no fixing it in post. If you are on her track at all,
  every word you both say at once is wasted.
- **Do not direct her.** The moment she is performing, it is a read take with
  extra steps.
- Budget 60-90 minutes to get 15-25 minutes of usable speech. Most of a
  gameplay recording is silence.

### Session B -- conversation, for coverage

Gameplay speech is emotionally right and phonetically narrow. Two hours of
"reloading" and "go left" does not cover the language.

So: same setup, same day, same mic, and just talk. Ask her about things she has
opinions about. Let her tell a story that takes five minutes. You want long,
relaxed, unselfconscious speech with a wide vocabulary.

### How much

A fine-tune on an existing VITS base transfers timbre and style, so it needs
far less than training from scratch. **30-60 minutes of clean, usable speech is
a reasonable target and more is better.** Treat that as a range to plan around,
not a threshold -- it depends on the base model and the pipeline, and the only
honest way to know is to train once and listen.

## Do a five-minute pilot first

Record five minutes. Run it all the way through segmentation, transcription and
whatever training pipeline you are going to use. Listen to the result.

**Do not run a two-hour session and then discover the mic was clipping**, or
that her headphones were leaking, or that the recorder was resampling to 16 kHz
mono with AGC on. Every one of those is invisible while recording and total
when found.

## Preparing the data -- the tools are already here

`build-catfight\Release\stt\` already carries what this needs:

| tool | does |
|---|---|
| `whisper-vad-speech-segments.exe` | splits a long recording into utterances |
| `whisper-cli.exe` | transcribes each one |

So the pipeline is:

1. **Segment** the raw session into utterances, roughly 1-15 seconds each.
2. **Transcribe** each segment.
3. **Correct the transcripts by hand.** This is tedious and it is the step that
   decides quality -- a model trained on wrong text learns to say the wrong
   sounds. Whisper also punctuates and capitalises, which may or may not be
   what the training pipeline wants.
4. **Drop anything** with game audio, two voices, clipping, or a transcript you
   are not sure of. A smaller clean set beats a larger dirty one.
5. **Resample** to whatever the pipeline wants -- Piper VITS is usually 22.05
   kHz mono.

## What ruins a dataset

In rough order of how often it happens:

- Recording the Discord stream instead of a local raw take
- Game audio leaking from speakers or open-back headphones
- Both of you talking at once on one track
- Gain set for conversation, clipped by shouting
- Mic distance drifting through the session
- Noise suppression or AGC left on in the recorder
- Transcripts left as Whisper produced them, uncorrected

## Before any of it: the contract

**A standard voice-over release licenses the recorded lines. That is not what
this is.**

You need, explicitly and in writing, the right to **synthesise new speech in
her voice, in perpetuity, including commercially**. She will say thousands of
things in this game that nobody ever recorded -- that is the entire point of
the system -- and a normal VO agreement does not grant it. Without that clause
you will have paid for audio you cannot ship.

`code/cf_game/VOICE.md` flags the same thing and notes that recording a friend
is viable and gives outright ownership. That is true, and it does not remove
the need for the clause. Write it down even when it is a friend -- especially
when it is a friend, because that is the relationship you least want to damage
later.

She should also know plainly what it is for: a game character, an AI companion
that generates her own lines, sold commercially. Informed is not optional here.
