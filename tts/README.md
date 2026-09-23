# cftts — speech synthesis, in a program of its own

`cftts.exe` turns a sentence into a WAV file. That is all it does and all it
knows how to do.

**This program is GPLv3.** Its source is here and it is published. The licence
text ships beside the binary as `tts/COPYING.txt`.

## Why it is a separate program

Not performance, and not tidiness. The licence.

- **sherpa-onnx is Apache 2.0** and would be perfectly comfortable linked into
  anything.
- **espeak-ng is GPLv3.** sherpa-onnx links it to turn text into the phonemes a
  VITS voice actually consumes — that is what `Vits.DataDir` is, and why
  `voice/espeak-ng-data` is 355 files of shipped data.
- GPLv3 reaches whatever it is linked into.

`cfvoice.exe` holds the persona, the memory schema, the prompts and the
guardrails. That is the most copyable thing this project owns and the hardest
to reproduce, and it is proprietary on purpose. Linking espeak-ng into it would
have required publishing all of it.

So the GPLv3 code lives out here, in a program that is nothing but a mouth. It
has no knowledge of the game, the player, the persona, the match, or anything
she has ever said. Everything it receives is one sentence and a filename.

This is the same argument, for the third time in this project:

| program | links | stays |
|---|---|---|
| `catfight_steam.exe` | the proprietary Steamworks SDK | out of the GPLv2 engine |
| `cfvoice.exe` | nothing of the engine's | proprietary |
| `cftts.exe` | GPLv3 espeak-ng | out of proprietary cfvoice |

Two programs at arm's length, exchanging text over a pipe, are two programs.
**This is not legal advice** — it is the shape a lawyer is being asked to
confirm, and it is deliberately the same shape all three times so that there is
one argument to confirm rather than three.

## Resident, not a command

Shelling out to the sherpa CLI costs about **1450 ms per line**, almost all of
it loading the model — against roughly **55 ms** of actual synthesis for 0.8 s
of audio. That cost is why the synthesiser was linked in via cgo in the first
place.

It is an argument against running *a program per sentence*, not against a
separate process. `cftts` loads the voice once at `hello` and stays up for the
session, so separation costs one pipe round trip rather than a model load.

## Protocol

One message per line, LF-terminated, over stdin and stdout. Every field that
can carry arbitrary text is hex encoded — a path can contain spaces and a line
protocol splits on them. The same convention `cl_voice.c` already uses.

```
->  hello 1                          the caller, immediately after spawning
<-  ready 1                          the voice is loaded
<-  unavailable <hex reason>         no voice; the caller degrades to text

->  speak <seq> <hex path> <hex text>
<-  speak <seq> ok                   the WAV is at that path
<-  speak <seq> err <hex reason>

->  quit
```

**The audio never crosses the pipe.** The caller names the file it wants and
this writes it, which is the convention `cl_voice.c` set for the same reason: a
few seconds of 16-bit mono is ~100 KB and there is nothing to gain by framing
it through a byte stream twice.

The file is written to `<path>.part` and renamed. Writing in place lets the
reader open a header with no samples behind it, which plays as a click and
looks like a synthesis bug.

Output is **16-bit mono WAV**, written by hand rather than through the
library's own writer, because the engine's playback path requires exactly that
and a helper that quietly produced something else would be *heard* as noise
rather than reported as an error.

stdout is the protocol; anything meant for a human goes to stderr, which the
parent inherits — so diagnostics land in cfvoice's log.

## Building

```powershell
.\tts\build.ps1
```

**This is the only part of the companion that needs a C compiler.** sherpa-onnx
is reached through cgo, so it wants the MinGW toolchain the engine already
requires. `cfvoice` itself no longer needs one — losing that dependency is a
side effect of this split, and a welcome one.

It builds into `build-catfight\Release\tts\` together with the three libraries
it loads. Those are prebuilt binaries that come down with the Go module, so
`build.ps1` copies them out of the module cache rather than committing them;
they are gitignored. The directory is its own for the ordinary Windows reason
as well — a DLL resolves from the directory of the exe that loads it, and
`onnxruntime.dll` has no business beside a `cfvoice.exe` that no longer uses
it.

## If the voice changes

Whether espeak-ng is needed at all is a property of **the voice model**, not of
sherpa:

- a VITS model trained on espeak phonemes needs it
- a model with a **lexicon** does not — sherpa takes `lexicon` in place of
  `DataDir`
- pre-recorded lines need no synthesiser at all

**"Unused" is not "unlinked."** If espeak-ng is still compiled into the DLL
being shipped, the obligation attaches whether or not anything calls it. Going
back to a single proprietary binary would mean building sherpa-onnx from source
with espeak-ng excluded — the Go module ships prebuilt libraries, so it is not
a flag — and confirming the result carries none of it.

Until then this separation is the cheap answer, and it costs one pipe.
