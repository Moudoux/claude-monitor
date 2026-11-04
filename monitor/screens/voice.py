"""Talk to Claude hands-free: it listens to the microphone, and a face and short replies appear on the display.

    python3 monitor/cli.py voice                   # just start talking; Ctrl+C to quit
    python3 monitor/cli.py voice --language en     # skip language detection, which is faster and more reliable
    python3 monitor/cli.py voice --source NAME     # another microphone, from `pactl list short sources`

Speech is turned into text on this computer with Whisper, so audio stays here; only the text goes to Claude.
The first run needs the speech package, and downloads the Whisper model:

    python3 -m venv .venv && .venv/bin/pip install faster-whisper

Anything the microphone hears counts as talking to Claude, and uses your Claude usage whenever Claude answers.
Because anyone or anything in the room can talk to it and nobody approves what it does, it can only reply and
search the web, with no access to your files, commands or connectors. --tools changes that, at your own risk.
The conversation lasts until you quit, and is not saved.

monitor/voice_context.txt tells Claude what it is and how to behave. Edit it to taste; it's read at startup.
"""

import collections
import json
import os
import queue
import random
import re
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
from pathlib import Path

from display import post, resolve_local

VENV = Path(__file__).resolve().parent.parent.parent / ".venv"
CONTEXT_FILE = Path(__file__).resolve().parent.parent / "voice_context.txt"  # who Claude is here; edit it freely

# What fits under the face: text size 2 is 25 characters wide, with room for 6 lines
COLUMNS = 25
ROWS = 6
PAGE_SECONDS = 6  # how long each page of a reply that doesn't fit stays up
FACE_COLOR = "#d97757"

IGNORE = "[ignore]"  # what Claude answers with when what it heard wasn't meant for it
SYSTEM_PROMPT = f"""\
You are talking with the user by voice. You hear them through an always-on microphone, \
transcribed by speech recognition, so expect the odd wrong word, and work out what they probably meant. Your \
replies appear on a tiny screen under a cartoon face, with room for {ROWS} lines of {COLUMNS} characters. So every \
reply must be one or two short spoken-style sentences, at most 20 words, in plain text: no markdown, lists, code \
or emoji. If what you hear is clearly not meant for you, such as a TV, a phone call or people talking to each \
other, reply with exactly {IGNORE} and nothing else."""

# Characters that the board's font doesn't have, and what to show instead
REPLACEMENTS = str.maketrans({"‘": "'", "’": "'", "“": '"', "”": '"', "–": "-", "—": "-", "…": "...",
                              "*": None, "`": None, "#": None})

FRAME_SECONDS = 0.12  # how often the animation looks for something to do
TALK_FRAME_SECONDS = 0.25
THINK_FRAME_SECONDS = 0.8
BLINK_SECONDS = 0.14
CHARS_PER_SECOND = 15  # how long the face "says" a reply
MAX_TALK_SECONDS = 8

# Finding speech in the microphone's audio: 16-bit mono at 16 kHz, looked at 30 ms at a time
SAMPLE_RATE = 16000
FRAME_SAMPLES = 480
FRAME_BYTES = FRAME_SAMPLES * 2
LOUD_RATIO = 3.5  # speech is this many times louder than the background noise...
MIN_LEVEL = 60  # ...and at least this loud (the level of a sample, out of 32768, as an RMS)
FLOOR_SPEED = 0.02  # how quickly the background noise level follows the room
LOUD_FLOOR_SPEED = 0.001  # the same during loud sounds, so they don't count as noise unless they last
START_FRAMES = 6  # this much loud audio in a row starts an utterance
END_FRAMES = 30  # this much quiet ends it
MIN_LOUD_FRAMES = 12  # with less loud audio than this in all, it was a noise and not speech
MAX_FRAMES = 1000  # 30 seconds, in case the noise never lets up
LEAD_IN_FRAMES = 15  # how much audio to keep from before it got loud, for the start of the first word

# What Whisper tends to "hear" in noise
PHANTOMS = {"", "you", "thank you", "thanks for watching", "bye", "okay"}
NO_SPEECH_LIMIT = 0.6


def add_arguments(parser):
    parser.add_argument("--language", help="the language you speak, like en or nl (default: detect it each time)")
    parser.add_argument("--source", help="the microphone, from `pactl list short sources` (default: the system's)")
    parser.add_argument("--model", default="haiku", help="the Claude model (default %(default)s, which is quick)")
    parser.add_argument("--whisper", default="base",
                        help="the Whisper model: tiny, base, small, medium... Bigger is slower and better "
                             "(default %(default)s)")
    parser.add_argument("--tools", default="WebSearch",
                        help="the Claude Code tools it may use, comma-separated (default %(default)s). "
                             "Nobody approves their use, and anyone in earshot can ask for it")


def check_args(parser, args):
    if args.schedule:
        parser.error("--schedule doesn't apply to this screen, which runs until Ctrl+C")


def system_prompt():
    """The rules for talking through the screen, after the context file if there is one."""
    context = CONTEXT_FILE.read_text().strip() if CONTEXT_FILE.exists() else ""
    return f"{context}\n\n{SYSTEM_PROMPT}".strip()


def screen_lines(text):
    """`text` as lines that fit the screen, in the characters the board can show."""
    text = re.sub(r"[^\x00-ɏ]", "", text.translate(REPLACEMENTS))  # past that are emoji and the like
    return textwrap.wrap(text, COLUMNS)


def document(face, *blocks):
    """The JSON document for a face with text under it. Each block is (lines, colour); what doesn't fit is left off."""
    lines = [{"text": line, "color": color} for block, color in blocks for line in block]
    return {"lines": [{"face": face, "color": FACE_COLOR}, *lines[:ROWS]]}


class Voice:
    """What the display should show, from the events that come in and the time."""

    def __init__(self):
        self.hint = screen_lines("I'm listening. Just talk to me.")
        self.state = "loading"
        self.reply = []
        self.status = screen_lines("Waking up...")
        self.heard = []
        self.state_since = self.next_blink = 0
        self.talk_seconds = 0

    def handle(self, name, text, now):
        if name == "ready" or name == "ignored":
            self.enter("idle", "", now)
        elif name == "speech":
            self.heard = []
            self.enter("listening", "Listening...", now)
        elif name == "transcribing":
            self.enter("thinking", "One moment...", now)
        elif name == "heard":
            self.heard = screen_lines(text)
            self.enter("thinking", "Thinking...", now)
        elif name == "tool":
            self.enter("thinking", f"Using {text}...", now)
        elif name == "failed":
            self.enter("error", text, now)
        elif name == "reply":
            self.reply = screen_lines(text)
            self.talk_seconds = min(sum(map(len, self.reply)) / CHARS_PER_SECOND, MAX_TALK_SECONDS)
            self.enter("talking", "", now)

    def enter(self, state, status, now):
        if state != self.state:
            self.state_since = now
        self.state = state
        self.status = screen_lines(status)

    def reply_page(self, now):
        pages = [self.reply[i:i + ROWS] for i in range(0, len(self.reply), ROWS)] or [self.hint]
        return pages[int((now - self.state_since) / PAGE_SECONDS) % len(pages)]

    def frame(self, now):
        """The JSON document for this moment. It only changes when there's something new to draw."""
        elapsed = now - self.state_since
        if self.state == "talking" and elapsed >= self.talk_seconds:
            self.state = "idle"  # the reply stays up, and its pages keep their timing
        if self.state == "loading":
            return document({"eyes": 0.3, "smile": 0.2}, (self.status, "grey"))
        if self.state == "listening":
            return document({"eyes": 1, "lookY": -0.4, "smile": 0.2, "mouth": 0.15}, (self.status, "grey"))
        if self.state == "thinking":
            look = 0.8 if int(elapsed / THINK_FRAME_SECONDS) % 2 else -0.8
            # What was heard, cut short if need be so that the status still fits below it
            heard = self.heard[:ROWS - len(self.status)]
            return document({"eyes": 0.8, "lookX": look, "lookY": -0.8}, (heard, "white"), (self.status, "grey"))
        if self.state == "error":
            return document({"eyes": 0.7, "smile": -0.8}, (self.status, "red"))
        if self.state == "talking":
            mouth = 0.8 if int(elapsed / TALK_FRAME_SECONDS) % 2 else 0.1
            return document({"smile": 0.3, "mouth": mouth}, (self.reply_page(now), "white"))
        return document({"eyes": self.eyes(now), "smile": 0.5}, (self.reply_page(now), "white"))

    def eyes(self, now):
        """Open, with a blink every few seconds."""
        if now >= self.next_blink + BLINK_SECONDS:
            self.next_blink = now + random.uniform(2.5, 6)
        return 0.05 if now >= self.next_blink else 1


def animate(voice, events, display, stop):
    """Runs until `stop` is set: takes in events, and posts a frame to the display when it has changed."""
    shown = None
    failing = False
    while not stop.is_set():
        try:
            voice.handle(*events.get(timeout=FRAME_SECONDS), time.monotonic())
        except queue.Empty:
            pass
        frame = voice.frame(time.monotonic())
        if frame != shown:
            try:
                post(display, frame)
                shown = frame
                failing = False
            except OSError as e:
                if not failing:
                    print(f"couldn't reach the display: {e}", file=sys.stderr)
                failing = True


class Segmenter:
    """Finds utterances in audio, from how loud it is compared with the room's background noise."""

    def __init__(self):
        self.floor = None
        self.reset()

    def reset(self):
        self.speaking = False
        self.loud_run = self.quiet_run = self.loud_total = 0
        self.frames = []
        self.lead_in = collections.deque(maxlen=LEAD_IN_FRAMES)

    def feed(self, frame):
        """Takes FRAME_BYTES of audio. Returns "speech" when an utterance starts, the audio of one (as bytes)
        when it ends, "noise" if it turns out not to have been speech, and otherwise None."""
        import numpy

        level = float(numpy.sqrt(numpy.mean(numpy.square(numpy.frombuffer(frame, numpy.int16).astype(float)))))
        if self.floor is None:
            self.floor = level
        loud = level > max(self.floor * LOUD_RATIO, MIN_LEVEL)

        if not self.speaking:
            self.floor += (LOUD_FLOOR_SPEED if loud else FLOOR_SPEED) * (level - self.floor)
            self.lead_in.append(frame)
            self.loud_run = self.loud_run + 1 if loud else 0
            if self.loud_run < START_FRAMES:
                return None
            self.speaking = True
            self.frames = list(self.lead_in)
            self.loud_total = self.loud_run
            self.quiet_run = 0
            return "speech"

        self.frames.append(frame)
        self.loud_total += loud
        self.quiet_run = 0 if loud else self.quiet_run + 1
        if self.quiet_run < END_FRAMES and len(self.frames) < MAX_FRAMES:
            return None
        audio = b"".join(self.frames)
        enough = self.loud_total >= MIN_LOUD_FRAMES
        self.reset()
        return audio if enough else "noise"


class Claude:
    """One Claude Code process for the whole conversation, which it remembers. It's started when first needed,
    and again if it has stopped, which loses the conversation so far."""

    def __init__(self, model, tools, folder):
        self.command = ["claude", "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
                        "--no-session-persistence", "--restricted", "--strict-mcp-config",
                        "--model", model, "--tools", tools, "--allowedTools", tools,
                        "--append-system-prompt", system_prompt()]
        self.folder = folder  # empty, so there's nothing there for Claude to read, or to pick up instructions from
        self.process = None

    def ask(self, text, on_tool):
        """Claude's reply to `text`. `on_tool` is called with the name of each tool it uses."""
        if self.process is None or self.process.poll() is not None:
            self.process = subprocess.Popen(self.command, cwd=self.folder, text=True, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        message = {"type": "user", "message": {"role": "user", "content": text}}
        try:
            self.process.stdin.write(json.dumps(message) + "\n")
            self.process.stdin.flush()
        except OSError as e:
            raise RuntimeError("Claude stopped unexpectedly.") from e
        for line in self.process.stdout:
            event = json.loads(line)
            if event.get("type") == "assistant":
                for block in event["message"]["content"]:
                    if block.get("type") == "tool_use":
                        on_tool(block["name"])
            elif event.get("type") == "result":
                if event.get("is_error"):
                    raise RuntimeError(str(event.get("result") or "Claude couldn't answer."))
                return event.get("result", "")
        raise RuntimeError("Claude stopped unexpectedly.")

    def close(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait()


def transcribe(model, audio, language):
    """The words in `audio`, or an empty string if it wasn't speech."""
    import numpy

    samples = numpy.frombuffer(audio, numpy.int16).astype(numpy.float32) / 32768
    segments, _ = model.transcribe(samples, language=language, vad_filter=True, beam_size=1,
                                   condition_on_previous_text=False)
    text = " ".join(s.text.strip() for s in segments if s.no_speech_prob < NO_SPEECH_LIMIT).strip()
    return "" if text.lower().strip(" .,!?") in PHANTOMS else text


def answer(audio, whisper, language, claude, events):
    """Takes one utterance through to the reply, telling the display how it's going."""
    events.put(("transcribing", ""))
    text = transcribe(whisper, audio, language)
    if not text:
        events.put(("ignored", ""))
        return
    print(time.strftime("%H:%M:%S"), "heard:", text, flush=True)
    events.put(("heard", text))
    reply = claude.ask(text, lambda tool: events.put(("tool", tool))).strip()
    print(time.strftime("%H:%M:%S"), "reply:", reply, flush=True)
    events.put(("ignored", "") if reply == IGNORE else ("reply", reply))


def respond(utterances, busy, whisper, language, claude, events):
    """Answers utterances from the `utterances` queue one at a time, until it gets None."""
    while (audio := utterances.get()) is not None:
        try:
            answer(audio, whisper, language, claude, events)
        except Exception as e:
            print(time.strftime("%H:%M:%S"), f"failed: {e}", file=sys.stderr, flush=True)
            events.put(("failed", "Something went wrong. See the terminal."))
        busy.clear()


def listen(stream, utterances, busy, events):
    """Passes what's said on `stream` to `utterances` until the audio ends. What's said while busy is ignored."""
    segmenter = Segmenter()
    while len(frame := stream.read(FRAME_BYTES)) == FRAME_BYTES:
        if busy.is_set():
            segmenter.reset()
            continue
        result = segmenter.feed(frame)
        if result == "speech":
            events.put(("speech", ""))
        elif result == "noise":
            events.put(("ignored", ""))
        elif result is not None:
            busy.set()
            utterances.put(result)


def use_venv():
    """Restarts this program with the speech package from .venv if it isn't available here. Returns whether it's
    available; if it's not and that can't help, it says what to do."""
    try:
        import faster_whisper  # noqa: F401
        return True
    except ImportError:
        pass
    python = VENV / "bin" / "python"
    if python.exists() and Path(sys.prefix).resolve() != VENV.resolve():
        os.execv(python, [str(python), *sys.argv])
    print("This screen needs the speech package. Install it from the project's folder with:\n"
          "    python3 -m venv .venv && .venv/bin/pip install faster-whisper", file=sys.stderr)
    return False


def main(args):
    """Listens and answers until Ctrl+C, or until the microphone stops. Returns the exit code."""
    if not use_venv():
        return 1
    from faster_whisper import WhisperModel

    display = resolve_local(args.display)
    voice = Voice()
    try:
        post(display, voice.frame(time.monotonic()))
    except OSError as e:
        print(f"couldn't reach the display: {e}", file=sys.stderr)
        return 1

    print(f"Loading the Whisper model '{args.whisper}' (the first time, this downloads it)...", flush=True)
    whisper = WhisperModel(args.whisper, device="cpu", compute_type="int8")

    record = ["parecord", "--raw", "--format=s16le", f"--rate={SAMPLE_RATE}", "--channels=1", "--latency-msec=30"]
    if args.source:
        record.append(f"--device={args.source}")
    events, utterances = queue.Queue(), queue.Queue()
    busy, stop = threading.Event(), threading.Event()
    with tempfile.TemporaryDirectory(prefix="claude-monitor-voice-") as folder:
        claude = Claude(args.model, args.tools, folder)
        threads = [threading.Thread(target=animate, args=(voice, events, display, stop)),
                   threading.Thread(target=respond, args=(utterances, busy, whisper, args.language, claude, events))]
        for thread in threads:
            thread.start()
        microphone = subprocess.Popen(record, stdout=subprocess.PIPE)
        events.put(("ready", ""))
        print("Listening. Just talk; Ctrl+C to quit.", flush=True)
        code = 0
        try:
            listen(microphone.stdout, utterances, busy, events)
            print("the microphone stopped; is parecord working?", file=sys.stderr)
            code = 1
        except KeyboardInterrupt:
            pass
        finally:
            microphone.terminate()
            utterances.put(None)
            claude.close()  # if it's in the middle of something, this ends it
            threads[1].join()
            stop.set()
            threads[0].join()
    try:
        post(display, document({"eyes": 0, "smile": 0.2}, (["Bye!"], "white")))
    except OSError as e:
        print(f"couldn't reach the display: {e}", file=sys.stderr)
    return code
