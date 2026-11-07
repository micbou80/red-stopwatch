"""Single-turn GPT-Live voice sessions and bounded USB audio transfers."""

import base64
import json
import math
import struct
import time

import websocket


VOICE_MODEL = "gpt-live-1"
SAMPLE_RATE = 24000
BYTES_PER_SECOND = SAMPLE_RATE * 2
CAPTURE_LIMIT = BYTES_PER_SECOND * 8
PLAYBACK_SECONDS = 30
PLAYBACK_LIMIT = BYTES_PER_SECOND * PLAYBACK_SECONDS
USB_CHUNK = 1024
FRAME_SECONDS = 0.04
FRAME_BYTES = int(BYTES_PER_SECOND * FRAME_SECONDS)


class VoiceError(RuntimeError):
    pass


def read_recording(watch, header):
    size = int(header.removeprefix(b"ORBIT MIC "))
    if not 0 < size <= CAPTURE_LIMIT or size % 2:
        raise VoiceError("Invalid microphone data length.")
    audio = bytearray()
    deadline = time.monotonic() + 10
    while len(audio) < size and time.monotonic() < deadline:
        audio.extend(watch.read(min(8192, size - len(audio))))
    if len(audio) != size:
        raise VoiceError("The microphone transfer timed out.")
    return bytes(audio)


def wait_for(watch, expected):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        reply = watch.readline().strip()
        if reply == expected:
            return
        if reply == b"ORBIT AUDIO ERROR":
            raise VoiceError("The watch could not play the audio.")
    raise VoiceError(f"The watch did not acknowledge {expected.decode()}.")


def play(watch, audio):
    if not 0 < len(audio) <= PLAYBACK_LIMIT or len(audio) % 2:
        raise VoiceError("Invalid speaker data length.")
    watch.write(f"PLAY {len(audio)}\n".encode())
    for offset in range(0, len(audio), USB_CHUNK):
        wait_for(watch, b"ORBIT RX")
        watch.write(audio[offset:offset + USB_CHUNK])
    wait_for(watch, b"ORBIT PLAYING")


def test_tone():
    return b"".join(
        struct.pack("<h", int(5000 * math.sin(2 * math.pi * 620 * n / SAMPLE_RATE)))
        for n in range(SAMPLE_RATE // 5)
    )


def ask_live(audio, key, backend_model):
    """Send one recording at audio rate, then collect one spoken reply."""
    ws = websocket.create_connection(
        "wss://api.openai.com/v1/live/sessions",
        header={"Authorization": f"Bearer {key}"},
        suppress_origin=True,
        timeout=8,
    )

    def send(kind, **fields):
        ws.send(json.dumps({"type": kind, **fields}))

    def receive():
        message = ws.recv()
        if not message:
            raise VoiceError("GPT-Live closed the connection.")
        event = json.loads(message)
        if event["type"] == "error":
            raise VoiceError(event.get("error", {}).get("message", "GPT-Live error."))
        return event

    started = False
    try:
        send("session.start", session={
            "model": VOICE_MODEL,
            "instructions": (
                "You are Red, a friendly pocket flower. Stay quiet until the user speaks. "
                "Answer in the user's language. Keep simple answers brief and give more detail "
                "when useful or requested. Delegate only if the question needs deeper reasoning."
            ),
            "audio": {
                "format": {"type": "audio/pcm", "rate": SAMPLE_RATE},
                "output": {"voice": "marin"},
            },
            "delegation": {"type": "responses", "responses": {"model": backend_model}},
        })
        if receive()["type"] != "session.started":
            raise VoiceError("GPT-Live did not start the voice session.")
        started = True
        reply = bytearray()
        transcript = []
        offset = 0
        silence = bytes(FRAME_BYTES)
        next_frame = time.monotonic()
        first_audio_deadline = next_frame + len(audio) / BYTES_PER_SECOND + 16
        deadline = first_audio_deadline + PLAYBACK_SECONDS
        last_reply_activity = None
        while (now := time.monotonic()) < deadline:
            # GPT-Live detects turns in the audio stream. Continue with silence
            # after the button is released so it can detect the end of speech.
            if now >= next_frame:
                frame = audio[offset:offset + FRAME_BYTES] or silence
                offset += min(FRAME_BYTES, len(audio) - offset)
                send("session.input_audio.append", audio=base64.b64encode(frame).decode())
                next_frame = now + FRAME_SECONDS
            if len(reply) >= PLAYBACK_LIMIT:
                break
            if not reply and now >= first_audio_deadline:
                break
            if reply and offset == len(audio) and now - last_reply_activity >= 2.2:
                break
            ws.settimeout(max(0.005, next_frame - time.monotonic()))
            try:
                event = receive()
            except websocket.WebSocketTimeoutException:
                continue
            if event["type"] == "session.output_audio.delta":
                reply.extend(base64.b64decode(event["delta"]))
                last_reply_activity = time.monotonic()
            elif event["type"] == "session.output_transcript.delta":
                transcript.append(event["delta"])
                last_reply_activity = time.monotonic()
        if not reply:
            raise VoiceError("GPT-Live returned no audio. Hold A until Listening appears and try again.")
        return "".join(transcript).strip(), bytes(reply[:PLAYBACK_LIMIT])
    finally:
        if started:
            try:
                ws.settimeout(2)
                send("session.close")
                close_deadline = time.monotonic() + 2
                while time.monotonic() < close_deadline:
                    if receive()["type"] == "session.closed":
                        break
            except (websocket.WebSocketException, OSError, VoiceError):
                pass
        ws.shutdown()
