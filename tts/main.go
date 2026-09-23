/*
cftts -- speech synthesis, in a program of its own.

WHY THIS EXISTS, AND IT IS NOT PERFORMANCE OR TIDINESS. It is the licence.

sherpa-onnx is Apache 2.0 and would be perfectly comfortable inside cfvoice.
What is not comfortable is espeak-ng: it is GPLv3, sherpa-onnx links it to turn
text into the phonemes a VITS voice actually consumes, and anything it is
linked into becomes GPLv3 too. cfvoice is where the persona, the memory schema,
the prompts and the guardrails live -- the most copyable thing the project owns
-- and it is proprietary on purpose. Linking espeak-ng into it would have
required publishing all of that.

So the GPLv3 code lives here instead, in a program that is nothing but a mouth:
text in, a WAV file out, no knowledge of the game, the player, the persona or
anything she has ever said. THIS BINARY IS GPLv3 AND ITS SOURCE IS PUBLISHED.
That is the whole price, and it buys keeping everything that matters closed.

It is the same argument, for the third time, that steam/README.md makes for the
Steam helper and cfvoice/README.md makes for cfvoice: two programs at arm's
length, speaking text over a pipe, are two programs. Not legal advice; it is
the shape a lawyer will be asked to confirm.

RESIDENT, NOT A COMMAND. Shelling out to the sherpa CLI costs ~1450ms per line
and almost all of it is loading the model -- the synthesis itself is ~55ms for
0.8 seconds of audio. That cost is why cfvoice used cgo in the first place, and
avoiding it is why this is a long-lived process that loads the voice once
rather than a program run per sentence. Separation costs a pipe round trip, not
a model load.

AUDIO GOES BY PATH, NOT DOWN THE PIPE, which is the convention cl_voice.c
already set for the same reason: a few seconds of 16-bit mono is ~100KB and
there is nothing to gain by pushing it through a byte stream both sides have to
frame. The caller names the file it wants; this writes it and says so.
*/
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	sherpa "github.com/k2-fsa/sherpa-onnx-go/sherpa_onnx"
)

// Protocol version. Bumped only if the verbs change meaning; adding a verb does
// not need it, because an old caller simply never sends the new one.
const protocolVersion = 1

type synth struct {
	tts   *sherpa.OfflineTts
	ready bool
}

/*
load brings up the voice.

A failure here is reported once, as `unavailable`, and the process stays alive
rather than exiting. cfvoice degrades to text when there is no voice -- she
still answers, she just does not speak -- and a helper that vanished would be
indistinguishable from one that was never installed.
*/
func (s *synth) load(model, tokens, dataDir string, threads int) error {
	for _, p := range []string{model, tokens, dataDir} {
		if _, err := os.Stat(p); err != nil {
			return fmt.Errorf("no voice at %s", p)
		}
	}

	cfg := sherpa.OfflineTtsConfig{}
	cfg.Model.Vits.Model = model
	cfg.Model.Vits.Tokens = tokens
	// espeak-ng's data directory. This field is the entire reason this program
	// is a separate GPLv3 binary -- see the header.
	cfg.Model.Vits.DataDir = dataDir
	cfg.Model.NumThreads = threads
	// One sentence at a time. The whole line is still synthesised, in pieces
	// small enough that a future streaming path can start playing the first
	// without waiting for the last.
	cfg.MaxNumSentences = 1

	t := sherpa.NewOfflineTts(&cfg)
	if t == nil {
		return fmt.Errorf("could not load the voice")
	}

	s.tts = t
	s.ready = true
	return nil
}

/*
speak synthesises text and writes it to path as 16-bit mono WAV.

Written by hand rather than through the library's own writer because the
engine's playback path requires exactly this format, and a helper that quietly
produced something else would be heard as noise rather than reported as an
error.
*/
func (s *synth) speak(text, path string) error {
	if !s.ready {
		return fmt.Errorf("no voice loaded")
	}

	audio := s.tts.Generate(text, 0, 1.0)
	if audio == nil || len(audio.Samples) == 0 {
		return fmt.Errorf("nothing was synthesised")
	}

	var buf bytes.Buffer
	dataBytes := len(audio.Samples) * 2

	buf.WriteString("RIFF")
	binary.Write(&buf, binary.LittleEndian, uint32(36+dataBytes))
	buf.WriteString("WAVEfmt ")
	binary.Write(&buf, binary.LittleEndian, uint32(16))                 // fmt size
	binary.Write(&buf, binary.LittleEndian, uint16(1))                  // PCM
	binary.Write(&buf, binary.LittleEndian, uint16(1))                  // mono
	binary.Write(&buf, binary.LittleEndian, uint32(audio.SampleRate))   //
	binary.Write(&buf, binary.LittleEndian, uint32(audio.SampleRate*2)) // bytes/sec
	binary.Write(&buf, binary.LittleEndian, uint16(2))                  // block align
	binary.Write(&buf, binary.LittleEndian, uint16(16))                 // bits
	buf.WriteString("data")
	binary.Write(&buf, binary.LittleEndian, uint32(dataBytes))

	// Float to 16-bit, CLAMPED. A synthesiser can overshoot 1.0 slightly, and
	// wrapping instead of clipping is heard as a crack rather than as a limit.
	for _, v := range audio.Samples {
		f := v * 32767.0
		if f > 32767.0 {
			f = 32767.0
		} else if f < -32768.0 {
			f = -32768.0
		}
		binary.Write(&buf, binary.LittleEndian, int16(f))
	}

	/*
		Written to a temporary file and renamed, because the reader is another
		process watching for the file to exist. Writing in place lets it open a
		header with no samples behind it yet, which plays as a click and looks
		like a synthesis bug.
	*/
	tmp := path + ".part"
	if err := os.WriteFile(tmp, buf.Bytes(), 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// Every field that can carry arbitrary text is hex encoded, for the same reason
// cl_voice.c does it: a path can contain spaces and a line protocol splits on
// them. Hex is not an encoding choice here, it is what keeps the framing.
func enc(s string) string { return hex.EncodeToString([]byte(s)) }

func dec(s string) (string, error) {
	b, err := hex.DecodeString(s)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

func main() {
	exe, err := os.Executable()
	if err != nil {
		exe = "."
	}
	exeDir := filepath.Dir(exe)

	/*
		Defaults resolve to the shipped layout, because cfvoice spawns this with
		no arguments at all. The voice lives one directory up: the DLLs sherpa
		needs have to sit beside THIS executable, which is why it has a
		directory of its own, but the model is content and is shared.
	*/
	var (
		model   = flag.String("model", filepath.Join(exeDir, "..", "voice", "model.onnx"), "VITS voice model")
		tokens  = flag.String("tokens", filepath.Join(exeDir, "..", "voice", "tokens.txt"), "voice tokens")
		espeak  = flag.String("espeak", filepath.Join(exeDir, "..", "voice", "espeak-ng-data"), "espeak-ng data")
		threads = flag.Int("threads", 4, "synthesis threads")
	)
	flag.Parse()

	// stdout is the protocol; anything said for a human goes to stderr. The
	// parent inherits it, so these land in cfvoice's own log.
	log.SetOutput(os.Stderr)
	log.SetFlags(0)
	log.SetPrefix("cftts: ")

	out := bufio.NewWriter(os.Stdout)
	say := func(format string, a ...interface{}) {
		fmt.Fprintf(out, format+"\n", a...)
		out.Flush()
	}

	var s synth

	in := bufio.NewScanner(os.Stdin)
	// A line is a command; none of them are long, but a hex-encoded sentence is
	// twice its own length and the default 64KB is not obviously enough forever.
	in.Buffer(make([]byte, 0, 64*1024), 1024*1024)

	for in.Scan() {
		line := strings.TrimSpace(in.Text())
		if line == "" {
			continue
		}
		parts := strings.Fields(line)

		switch parts[0] {
		case "hello":
			/*
				The model is loaded HERE rather than at startup, so that the
				caller learns the answer as a protocol reply instead of having
				to infer it from how long the process took to say anything.
			*/
			if err := s.load(*model, *tokens, *espeak, *threads); err != nil {
				log.Printf("%v", err)
				say("unavailable %s", enc(err.Error()))
				continue
			}
			log.Printf("voice ready (%s)", filepath.Base(*model))
			say("ready %d", protocolVersion)

		case "speak":
			// speak <seq> <hex path> <hex text>
			if len(parts) < 4 {
				continue
			}
			seq, err := strconv.Atoi(parts[1])
			if err != nil {
				continue
			}
			path, err1 := dec(parts[2])
			text, err2 := dec(parts[3])
			if err1 != nil || err2 != nil {
				say("speak %d err %s", seq, enc("unreadable request"))
				continue
			}
			if err := s.speak(text, path); err != nil {
				log.Printf("speak: %v", err)
				say("speak %d err %s", seq, enc(err.Error()))
				continue
			}
			say("speak %d ok", seq)

		case "quit":
			return

		default:
			// Unknown verbs are ignored rather than fatal: a newer cfvoice
			// talking to an older helper should lose the feature, not the voice.
			log.Printf("ignoring %q", parts[0])
		}
	}

	// stdin closed: the parent is gone. Nothing to synthesise for.
	if s.tts != nil {
		sherpa.DeleteOfflineTts(s.tts)
	}
}
