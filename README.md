# Red for M5Stack StopWatch

<img src="assets/orbit-flower.png" alt="Red, the flower avatar" width="128">

**Firmware 0.3.0** · Voice assistant and timer for the round M5Stack StopWatch (C152)

Red is a pocket-sized flower you can talk to. Hold the left button to ask a question, read the live transcript on the watch, and hear the reply from its speaker. Once Wi-Fi is set up, the watch connects directly to GPT-Live. You can unplug USB and use Red on battery.

## What Red can do

| Feature | What it does |
| --- | --- |
| Voice chat | Streams your transcript and shows the answer as text and audio arrive. Red carries up to four recent question-and-answer pairs across standalone turns. |
| Live information | Delegates questions about current facts to web search. Try “What’s tomorrow’s weather in Berlin?” |
| Home Assistant | Query exposed device states, control lights, fans, and switches, and open an exposed camera's live view by voice. |
| Voice timer | Starts, checks, extends, and cancels a local countdown. The full-size timer view shows `HH:MM:SS`, with a vibration and chime at zero. |
| Hands-free mode | Double-click **A** to turn on automatic listening. By default, listening resumes after the spoken reply ends. |
| Round-screen UI | The flower bounces when you tap the home screen. Swipe to scroll long answers. White status icons show battery percentage, charging, and Wi-Fi strength. |
| Settings | The bottom red button opens Wi-Fi, Bluetooth setup, saved speaker volume, hands-free mode, and the experimental voice interruption toggle. The firmware version appears below **Back**. |
| USB demo | The optional Python bridge provides offline mic loopback and canned replies, live voice with a key, typed questions, and a display and speaker smoke test. |

## Get started

You’ll need an M5Stack StopWatch C152, a USB-C data cable, a computer with Python, a 2.4 GHz Wi-Fi network, and an OpenAI project API key for live features. For a new watch, install the firmware using [Build and flash](#build-and-flash) first. The commands below use PowerShell.

```powershell
python -m pip install pyserial websocket-client
python orbit.py --setup-watch
```

1. Connect the watch over USB and close any running Red bridge before launching setup.
2. The watch shows up to five visible 2.4 GHz networks, strongest first. Press **A** (left) to move and press **A+B** together to select. In the terminal, press Enter to use that choice or type the network name directly.
3. Enter the Wi-Fi password and OpenAI project API key at the hidden prompts. On subsequent setups, press Enter at the key prompt to keep the saved key.
4. Wait for the connection check to pass. Unplug USB, hold **A** until **Listening** appears, speak, then release.

Red scans automatically at first setup or after waiting about 12 seconds to join a saved network. You can also scan from Settings. If the watch shows **Wi-Fi joined. Syncing clock**, it is setting its clock for the secure connection.

Use a dedicated project key for the watch. The API key and Wi-Fi password are stored in local device flash, where someone with physical access can read them. The setup script keeps these credentials out of the repository. Live features require internet access and use your OpenAI project. [GPT-Live model and pricing](https://developers.openai.com/api/docs/models/gpt-live-1)

## Pair a phone over Bluetooth

Use Bluetooth setup to send a new Wi-Fi network to Red from a phone. Red keeps its saved OpenAI key. Use the USB setup above for the initial API key.

1. Press the bottom red button to open Settings. Press **A** to highlight **Bluetooth setup**, then press **A+B** together to open it.
2. Open **ESP BLE Provisioning** on the phone and scan the QR code on Red. For manual setup in the app, look for the `PROV_RED_…` device and enter the code shown on the watch. [iPhone and iPad](https://apps.apple.com/app/esp-ble-provisioning/id1473590141) · [Android](https://play.google.com/store/apps/details?id=com.espressif.provble)
3. Choose a 2.4 GHz Wi-Fi network in the app and enter its password. Wait for **Wi-Fi saved** on Red, then press **B** to return to Settings.

Pairing uses a fresh code and closes after two minutes. Press **B** to cancel. If the network fails to join, press **A+B** to retry. Red saves the new network after a successful connection and restores the previous network on cancel or timeout. [Espressif’s BLE provisioning guide](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/provisioning/wifi_provisioning.html)

## Controls

| Control | What it does |
| --- | --- |
| Hold **A**, then release | Ask one question. Hold **A** again while Red is speaking to interrupt and ask another. |
| Double-click **A** | Toggle hands-free mode in standalone use. |
| Press **B** | Go back one screen. From a question, reply, timer, or camera view, return to the avatar. A running timer continues counting. |
| Press the bottom red button | Open or close Settings, or go back in a setup screen. From outside a menu, hold **A+B** to open Settings. |
| Menus | Press **A** for Next, **B** for Back, and **A+B** together to Select. Volume cycles through 25%, 50%, 75%, and 100%. |
| **Display** in Settings | Cycle through 30 seconds (default), 1 minute, 5 minutes, and Never. The watch saves your choice. Pick up Red, touch the screen, or press a button to wake it. Voice activity and timer alerts also wake the display. |
| Tap or swipe | Tap the home screen to make Red bounce. Swipe up or down on a long reply to read more. |

The watch records up to about eight seconds per question and buffers up to about 30 seconds of reply audio. Conversation context clears on restart. **Voice interrupt beta** allows simultaneous mic and speaker audio in hands-free mode. It defaults to off while we improve echo handling and speech capture.

## Try the timer

Hold **A** and say “Set a timer for seven minutes.” Red confirms the start and opens the countdown on the same canvas used for replies and transcripts. Try “How much time is left?”, “Add one minute,” or “Cancel the timer” on the next turn.

Red supports one timer from one second to 24 hours. A second start request leaves a running timer in place. At expiry, `00:00:00` turns orange and stays visible on the timer screen. The watch vibrates and plays a chime, which waits if the speaker is busy. Once a timer starts, it runs on the watch even if Wi-Fi drops. Keep the watch powered on.

## How it works

In standalone mode, the ESP32-S3 streams microphone audio directly to GPT-Live 1 over Wi-Fi. GPT-Live sends back transcription, reply text, and speech. GPT-6 Luna handles delegated web searches and can call the watch’s timer tool. The firmware applies timer commands locally and returns the actual result before Red confirms an action.

## Home Assistant from anywhere

1. Enable the **Model Context Protocol** integration in Home Assistant. Use the **Device control** MCP endpoint, which ends in `/api/mcp/assist`. Expose only the lights, fans, switches, and sensors you want Red to use. Home Assistant Cloud provides the public HTTPS address. [Home Assistant MCP setup](https://www.home-assistant.io/integrations/mcp_server/)
2. Create a dedicated Home Assistant long-lived access token from the account’s **Profile → Security** page. A non-admin account can use the Device control endpoint. [Home Assistant access tokens](https://www.home-assistant.io/docs/authentication/)
3. Connect Red by USB and close any running bridge. Run the setup command and enter the Home Assistant Cloud URL and access token at the hidden prompts.

```powershell
python orbit.py --setup-hass
```

Setup checks the remote MCP tool list and saves the supported tools to Red. Try “Is the living room lamp on?” or “Turn off the living room lamp” with the name used in Home Assistant. Red uses this integration in standalone voice mode from any internet-connected Wi-Fi network.

For a camera, expose it to **Assist** and ask “Show the garden camera.” Red uses the Device control MCP endpoint to discover the camera by name or area. It shows a full-screen preview, then opens the Home Assistant image stream over HTTPS with a one-second update interval. If the stream is unavailable, Red refreshes the preview with snapshots. Press **B** to close the feed and return home. The images go directly from Home Assistant to Red.

The watch stores the endpoint and token in local flash. GPT-Live passes the token to OpenAI’s Responses backend to authenticate the MCP calls. Remove the saved settings with `python orbit.py --clear-hass` and revoke the token in Home Assistant if you no longer need it. [GPT-Live MCP tool docs](https://developers.openai.com/api/docs/guides/live-delegation#add-mcp-and-other-tools)

| Path | What lives there |
| --- | --- |
| [`src/main.cpp`](src/main.cpp) | Screens, buttons and touch, microphone and speaker playback, settings, and timer alerts |
| [`src/orbit_live.cpp`](src/orbit_live.cpp) | GPT-Live WebSocket, captions, recent conversation context, and delegated tool results |
| [`src/red_timer.cpp`](src/red_timer.cpp) | Local timer state and countdown |
| [`src/wifi_picker.cpp`](src/wifi_picker.cpp) | Wi-Fi scan and on-watch selection |
| [`src/bluetooth_setup.cpp`](src/bluetooth_setup.cpp) | Phone pairing and Wi-Fi provisioning over BLE |
| [`src/duplex_audio.cpp`](src/duplex_audio.cpp) | Experimental simultaneous capture and playback with echo cancellation |
| [`assets/`](assets/) | Flower artwork and embedded Roboto bitmap fonts |
| [`orbit.py`](orbit.py) and [`orbit_voice.py`](orbit_voice.py) | USB provisioning, demo and smoke test, and optional live voice bridge |
| [`tools/push_before_upload.py`](tools/push_before_upload.py) | GitHub sync before firmware upload |

## USB demo and diagnostics

Run these from the repository with the watch connected. Install the Python dependencies from [Get started](#get-started) first.

```powershell
python orbit.py --demo
python orbit.py --smoke-test
python orbit.py --ask-key
```

- **Demo** loops back what the mic captured and provides canned replies for typed questions. It works without an API key.
- **Smoke test** sends a sample reply and a short tone to check the display and speaker transport.
- **Live bridge** uses a hidden API key prompt, or the `OPENAI_API_KEY` environment variable. Voice sessions start after you release **A**. You can also type a question in the terminal for a GPT-6 Luna text reply. The watch shows a bounded copy of the terminal text.

Run one mode at a time. Use `/quit` or Ctrl+C to close the bridge. If autodetection finds multiple Espressif devices, pass `--port COM7` or substitute the watch’s current port.

## Build and flash

Install Git and authenticate to the GitHub remote. Commit the changes you want on the watch and close the USB bridge before uploading.

```powershell
python -m pip install platformio pyserial websocket-client
python -m platformio run
python -m serial.tools.list_ports -v
python -m platformio run --target upload --upload-port COM7
```

Replace `COM7` with the watch’s port. A local build works without pushing. The upload target requires a clean Git branch and pushes the built source revision to the matching branch on `origin` before writing to the watch. A failed push stops the upload.

If the uploader cannot connect, leave USB attached and hold the power button until the green LED blinks. Release it, list the ports again, and retry with the reported port. [M5Stack StopWatch documentation](https://docs.m5stack.com/en/core/StopWatch)

### Fonts and dependencies

Red uses native-size 88 px Roboto Regular for the timer, 32 px for transcripts and replies, 24 px for titles and settings options, and 16 px for labels. Conversation text uses a built-in fallback when it needs glyphs outside the embedded subset. To regenerate the bitmaps, install Pillow and fonttools and run `python assets/build_transcript_font.py path/to/Roboto-Regular.ttf` with the hinted static font. [Roboto v3.015](https://github.com/googlefonts/roboto-3-classic/releases/tag/v3.015) · [Roboto license](assets/Roboto-OFL.txt)

The experimental echo cancellation path uses the bundled SpeexDSP 1.2.1 code. [SpeexDSP license](lib/speexdsp/COPYING)

### Restore the original flash

The original demo watch has a local backup at `%LOCALAPPDATA%\OrbitStopWatch\stopwatch-backup.bin`. Use the backup only on the watch it came from. Close the bridge and substitute the current serial port.

```powershell
python "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM7 --baud 921600 write_flash 0x0 "$env:LOCALAPPDATA\OrbitStopWatch\stopwatch-backup.bin"
```

## What’s next

- Add a local timezone setting for time-sensitive questions and future calendar alerts.
- Make hands-free transcription and interruption more reliable.
- Revisit battery indicator placement.
- Explore named timers, read-only calendar lookups with reminders, and a shared task list.
- Revisit app-free Wi-Fi setup from an iPhone when Red needs to join a new network.
