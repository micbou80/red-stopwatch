"""USB bridge for the Red StopWatch demo."""

import argparse
import getpass
from http.client import HTTPException, HTTPSConnection
import json
import os
import queue
import threading
import time
import unicodedata
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from urllib.parse import urlsplit, urlunsplit

import serial
import websocket
from serial.tools import list_ports

from orbit_voice import VOICE_MODEL, VoiceError, ask_live, play, read_recording, test_tone


MODEL = "gpt-6-luna"
TAP_QUESTION = "Tell me a tiny, surprising fact about space."
DEMO_ANSWERS = (
    "On Venus, a day lasts longer than a year!",
    "Saturn's rings are mostly chunks of ice.",
    "A year on Mercury takes just 88 Earth days.",
)


def ask_openai(question, key):
    payload = {
        "model": MODEL,
        "instructions": (
            "You are Red, a friendly pocket flower. Answer in the user's language "
            "using plain Latin characters where possible. Give one useful sentence "
            "of at most 75 characters. Use plain text, no emoji or Markdown."
        ),
        "input": question,
        "reasoning": {"effort": "none"},
        "max_output_tokens": 100,
        "store": False,
    }
    request = Request(
        "https://api.openai.com/v1/responses",
        data=json.dumps(payload).encode(),
        headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
    )
    with urlopen(request, timeout=20) as response:
        result = json.load(response)
    answer = " ".join(
        part.get("text", "") or part.get("refusal", "")
        for item in result.get("output", [])
        if item.get("type") == "message"
        for part in item.get("content", [])
    ).strip()
    if not answer:
        raise ValueError("The model returned no answer.")
    return answer


def send(watch, command, message=""):
    # The built-in display font and serial protocol use printable ASCII.
    message = message.translate(str.maketrans({"’": "'", "“": '"', "”": '"', "—": "-"}))
    message = unicodedata.normalize("NFKD", message).encode("ascii", "ignore").decode()
    message = " ".join("".join(c for c in message if c.isprintable() or c.isspace()).split())
    if len(message) > 760:
        message = message[:757] + "..."
    suffix = f" {message or '(No displayable text)'}" if command in ("SAY", "ERROR") else ""
    watch.write(f"{command}{suffix}\n".encode())


def connect(watch, demo):
    for _ in range(12):
        send(watch, "HELLO DEMO" if demo else "HELLO LIVE")
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            reply = watch.readline().strip()
            if reply == b"ORBIT READY":
                return
            if reply.startswith(b"ORBIT ERROR "):
                raise RuntimeError(reply.decode(errors="replace"))
    raise RuntimeError("Red firmware did not answer on this port.")


def read_questions(questions):
    try:
        for line in iter(input, "/quit"):
            if line.strip():
                questions.put(line.strip()[:500])
    except EOFError:
        pass
    finally:
        questions.put(None)


def check_cloud(watch):
    watch.write(b"WIFI CHECK\n")
    deadline = time.monotonic() + 17
    api_error = ""
    while time.monotonic() < deadline:
        response = watch.readline().strip()
        if response == b"ORBIT CLOUD READY":
            print("GPT-Live accepted the watch's connection.")
            return True
        if response.startswith(b"ORBIT LIVE API ERROR "):
            api_error = response.removeprefix(b"ORBIT LIVE API ERROR ").decode(errors="replace")
        if response in (b"ORBIT CLOUD FAILED", b"ORBIT CLOUD UNAVAILABLE"):
            break
    print("Wi-Fi is joined. The GPT-Live connection check failed." +
          (f" OpenAI error: {api_error}." if api_error else ""))
    return False


def setup_watch(port):
    print("Red Wi-Fi setup. Use a dedicated OpenAI project key for this watch.")
    print("The watch saves the Wi-Fi password and API key in its local flash.")
    print("Device flash is readable by someone with physical access.")
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as watch:
        watch.write(b"WIFI SCAN\n")
        print("On the watch, press Left to move and Right to select your network.")
        while True:
            ssid = input("Press Enter to use the watch selection, or type a 2.4 GHz network name: ")
            if ssid:
                break
            watch.write(b"WIFI PICK\n")
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                response = watch.readline().strip()
                if response.startswith(b"ORBIT WIFI PICK "):
                    ssid = json.loads(response.removeprefix(b"ORBIT WIFI PICK "))["ssid"]
                    break
            if ssid:
                print(f"Selected network: {ssid!r}")
                break
            print("Select a network on the watch first, or enter its name here.")
    password = getpass.getpass("Wi-Fi password: ")
    key = getpass.getpass("OpenAI project API key (Enter to keep saved key): ").strip()
    if not 0 < len(ssid.encode()) <= 32 or len(password.encode()) > 63:
        raise RuntimeError("The Wi-Fi network name or password is too long.")
    if key and (not key.startswith("sk-") or len(key) > 250):
        raise RuntimeError("Enter a valid OpenAI project API key.")
    settings = json.dumps(
        {"ssid": ssid, "password": password, "key": key}, ensure_ascii=False,
    ).encode()
    if len(settings) + 9 > 768:
        raise RuntimeError("The settings are too long for the watch.")
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as watch:
        watch.write(b"WIFI SET " + settings + b"\n")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            reply = watch.readline().strip()
            if reply == b"ORBIT WIFI SAVED":
                break
            if reply == b"ORBIT WIFI ERROR":
                raise RuntimeError("The watch rejected the settings. An API key is required on first setup.")
        else:
            raise RuntimeError("The watch did not acknowledge Wi-Fi setup.")
    print("Settings saved. Waiting for the watch to join Wi-Fi...", flush=True)
    time.sleep(2)
    deadline = time.monotonic() + 25
    status = b""
    while time.monotonic() < deadline:
        try:
            with serial.Serial(port, 115200, timeout=0.4, write_timeout=2) as watch:
                watch.write(b"WIFI STATUS\n")
                response = watch.readline().strip()
                if response == b"ORBIT WIFI READY":
                    print("Red is on Wi-Fi. Checking GPT-Live...", flush=True)
                    if check_cloud(watch):
                        print("Unplug USB and hold the left button to talk.")
                    return
                if response in (b"ORBIT WIFI CLOCK", b"ORBIT WIFI WAIT"):
                    status = response
        except serial.SerialException:
            pass
        time.sleep(0.5)
    if status == b"ORBIT WIFI CLOCK":
        print("Wi-Fi joined. Red is still syncing its clock for the secure GPT-Live connection.")
    else:
        print("Settings saved. The watch has not joined Wi-Fi. Check the network and password.")


HASS_PATH = "/api/mcp/assist"
HASS_TOOLS = {"GetLiveContext", "HassGetState", "HassTurnOn", "HassTurnOff", "HassLightSet"}


def hass_endpoint(value):
    parts = urlsplit(value)
    if (parts.scheme != "https" or not parts.hostname or parts.username is not None
            or parts.password is not None or parts.query or parts.fragment
            or parts.path.rstrip("/") not in ("", HASS_PATH)):
        raise RuntimeError("Enter the public HTTPS Home Assistant Cloud URL or its Device control MCP URL.")
    return urlunsplit(parts._replace(path=HASS_PATH))


def hass_tools(endpoint, token):
    parts = urlsplit(endpoint)
    connection = HTTPSConnection(parts.hostname, parts.port or 443, timeout=12)
    try:
        connection.request(
            "POST", parts.path,
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list"}),
            {
                "Authorization": f"Bearer {token}",
                "Content-Type": "application/json",
                "Accept": "application/json, text/event-stream",
            },
        )
        response = connection.getresponse()
        if response.status in (401, 403):
            raise RuntimeError("Home Assistant rejected the token or its Device control permissions.")
        if response.status == 404:
            raise RuntimeError("Home Assistant did not find the Device control MCP endpoint. Enable MCP in Settings.")
        if response.status != 200:
            raise RuntimeError(f"Home Assistant MCP returned HTTP {response.status}.")
        content_type = response.getheader("Content-Type", "")
        payload = response.read(256001)
        if len(payload) > 256000:
            raise RuntimeError("Home Assistant returned an unexpectedly large tool list.")
        if content_type.startswith("text/event-stream"):
            messages = [
                json.loads(line[5:].strip())
                for line in payload.decode().splitlines() if line.startswith("data:")
            ]
            result = next((item for item in messages if item.get("id") == 1), {})
        else:
            result = json.loads(payload)
        names = {}
        for tool in result.get("result", {}).get("tools", []):
            name = tool.get("name", "")
            base = name.removeprefix("assist__")
            base = base.removeprefix("homeassistant__").removeprefix("intent__")
            if base in HASS_TOOLS:
                names[base] = name
        if not names:
            raise RuntimeError("Home Assistant MCP did not list any supported Device control tools.")
        return sorted(names.values())
    except (OSError, HTTPException, ValueError) as error:
        raise RuntimeError("Could not read the Home Assistant MCP tool list over HTTPS.") from error
    finally:
        connection.close()


def setup_hass(port, clear=False):
    command = b"HASS OFF\n"
    if not clear:
        endpoint = hass_endpoint(getpass.getpass("Home Assistant Cloud HTTPS URL: ").strip())
        token = getpass.getpass("Home Assistant long-lived access token: ").strip()
        if not token or len(token) > 512 or any(ch not in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-" for ch in token):
            raise RuntimeError("Enter a valid Home Assistant long-lived access token.")
        names = hass_tools(endpoint, token)
        command = ("HASS SET " + json.dumps({"url": endpoint, "token": token, "tools": names}, separators=(",", ":")) + "\n").encode()
        if len(command) - 1 > 768:
            raise RuntimeError("The Home Assistant settings are too long for the watch.")
        print(f"Home Assistant MCP listed {len(names)} supported tools. Saving to Red...", flush=True)
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as watch:
        watch.write(command)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            reply = watch.readline().strip()
            if reply == b"ORBIT HASS SAVED":
                print("Home Assistant disconnected from Red." if clear else "Home Assistant saved. Try asking Red about an exposed light.")
                return
            if reply == b"ORBIT HASS BUSY":
                raise RuntimeError("Red is busy. Close the USB bridge or wait for the current turn to finish.")
            if reply == b"ORBIT HASS ERROR":
                raise RuntimeError("Red rejected the Home Assistant settings.")
    raise RuntimeError("Red did not acknowledge Home Assistant setup. Check the firmware version and close any setup menu.")


def run(watch, key, demo, smoke_test):
    connect(watch, demo)
    if smoke_test:
        send(watch, "SAY", DEMO_ANSWERS[0])
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if watch.readline().strip() == b"ORBIT SHOWN":
                play(watch, test_tone())
                print("Red acknowledged the demo reply and started the speaker test tone.")
                return
        raise RuntimeError("Red did not acknowledge the demo reply.")

    print("Red connected. " + (
        "Offline mic loopback and canned text replies." if demo
        else f"AI-generated voice with {VOICE_MODEL}. Typed questions use {MODEL}."
    ))
    print("Hold A until Listening appears, then speak and release. Press B to replay.")
    print("Type a question and press Enter, or type /quit.")
    questions = queue.Queue()
    threading.Thread(target=read_questions, args=(questions,), daemon=True).start()
    demo_index = 0
    recording = False
    while True:
        event = watch.readline().strip()
        if event == b"ORBIT RECORDING":
            recording = True
            continue
        if event == b"ORBIT MIC SHORT":
            recording = False
            continue
        if event.startswith(b"ORBIT MIC "):
            recording = False
            try:
                audio = read_recording(watch, event)
                if demo:
                    answer = "Mic check: here's your voice."
                else:
                    answer, audio = ask_live(audio, key, MODEL)
                    answer = answer or "Voice reply"
                send(watch, "SAY", answer)
                play(watch, audio)
                print(f"Red: {answer}", flush=True)
            except (VoiceError, ValueError, websocket.WebSocketException, OSError) as error:
                print(f"Voice reply failed: {error}", flush=True)
                send(watch, "ERROR", "Voice error. See PC.")
            continue
        if recording:
            continue
        if event == b"ORBIT ASK":
            question = TAP_QUESTION
        else:
            try:
                question = questions.get_nowait()
            except queue.Empty:
                continue
            if question is None:
                send(watch, "BYE")
                return
        send(watch, "THINK")
        try:
            if demo:
                time.sleep(0.65)
                answer = DEMO_ANSWERS[demo_index % len(DEMO_ANSWERS)]
                demo_index += 1
            else:
                answer = ask_openai(question, key)
            send(watch, "SAY", answer)
            print(f"Red: {answer}", flush=True)
        except (HTTPError, URLError, TimeoutError, ValueError) as error:
            print(f"OpenAI request failed: {error}", flush=True)
            send(watch, "ERROR", "API error. See PC.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="StopWatch serial port, e.g. COM7")
    parser.add_argument("--demo", action="store_true", help="Use local mic loopback and canned text")
    parser.add_argument("--ask-key", action="store_true", help="Prompt for a live API key without echoing it")
    setup = parser.add_mutually_exclusive_group()
    setup.add_argument("--setup-watch", action="store_true", help="Set up standalone Wi-Fi mode over USB")
    setup.add_argument("--setup-hass", action="store_true", help="Set up remote Home Assistant MCP over USB")
    setup.add_argument("--clear-hass", action="store_true", help="Remove Home Assistant credentials from Red")
    parser.add_argument("--smoke-test", action="store_true", help="Test the watch display and speaker transport")
    args = parser.parse_args()
    demo = args.demo or args.smoke_test
    key = os.getenv("OPENAI_API_KEY", "").strip()
    provisioning = args.setup_watch or args.setup_hass or args.clear_hass
    if args.ask_key and not (key or demo or provisioning):
        key = getpass.getpass("OpenAI API key: ").strip()
    if not (key or demo or provisioning):
        parser.error("Set OPENAI_API_KEY, use --ask-key for live answers, or use --demo.")
    port = args.port
    if not port:
        matches = [p.device for p in list_ports.comports() if (p.vid, p.pid) == (0x303A, 0x1001)]
        if len(matches) != 1:
            parser.error("Specify the StopWatch serial port with --port COMx.")
        port = matches[0]
    try:
        if args.setup_watch:
            setup_watch(port)
            return
        if args.setup_hass or args.clear_hass:
            setup_hass(port, clear=args.clear_hass)
            return
        with serial.Serial(port, 115200, timeout=0.1, write_timeout=2) as watch:
            try:
                run(watch, key, demo, args.smoke_test)
            except KeyboardInterrupt:
                send(watch, "BYE")
    except (serial.SerialException, RuntimeError) as error:
        parser.exit(1, f"{port}: {error}\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
