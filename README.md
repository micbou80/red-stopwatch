# Red for M5Stack StopWatch

Red is a tiny voice assistant for the round M5Stack StopWatch (C152). Speak into the watch and it answers through its own speaker, with a short caption on screen. The watch connects to GPT-Live 1 over Wi-Fi. GPT-Live can delegate harder questions to GPT-6 Luna.

## Set up standalone Wi-Fi

Connect the watch to Windows by USB for setup. Close any running Red bridge, then run these commands from this folder:

```powershell
python -m pip install pyserial websocket-client
python orbit.py --setup-watch
```

The watch shows up to five visible 2.4 GHz networks, strongest first. Press **Left** to move the highlight and **Right** to choose. Press Enter in the setup window to use that selection, or type a network name directly. Enter the Wi-Fi password and an OpenAI project API key. If a key is already saved, press Enter at the key prompt to keep it. The password and key prompts hide what you type. Use a key dedicated to this watch. The watch saves both credentials in local flash, which someone with physical access can read.

The scan opens automatically on first setup or after the watch has waited 12 seconds for its saved network. You can also press **Right** from the home screen while Wi-Fi is unavailable. Press the bottom red button to go back. If the watch joins Wi-Fi but still needs to set its clock, it shows **Wi-Fi joined. Syncing clock**.

Once the terminal confirms Wi-Fi, unplug USB and hold the left button until **Listening** appears. Keep holding through the whole question. The watch switches from the avatar to a live transcript, then streams the answer text and audio as they arrive. The watch requires internet access for the AI models.

Red can play and replay up to about 30 seconds of reply audio. Answer length depends on the question, and longer captions scroll on the watch.

The watch uses Roboto Regular at 88 px for the timer, 32 px for transcripts and replies, 24 px for titles and settings options, and 16 px for labels. Conversation text can use the built-in font when it needs glyphs outside the embedded Roboto subset. To regenerate the bitmaps, install Pillow and fonttools and run `python assets/build_transcript_font.py path/to/Roboto-Regular.ttf` using the hinted static font from [Roboto v3.015](https://github.com/googlefonts/roboto-3-classic/releases/tag/v3.015). The SIL Open Font License is in `assets/Roboto-OFL.txt`.

## Standalone controls

- **Hold A** to ask one question. Release A when you finish. You can hold A again while Red is speaking to interrupt and ask a new question.
- **Double-click A** to toggle hands-free mode. Give the mic a moment to start, then speak without holding a button. Hands-free listening resumes after Red finishes speaking. Hands-free mode uses more battery while idle.
- **Press B** to stop a reply or replay the last completed reply when idle. **Hold B** on a completed reply to go home.
- **Press the bottom red button** to open or close settings. Holding A and B together also opens it. Press A to move and B to select. You can scan for Wi-Fi, change the saved volume in 25% steps, or toggle hands-free mode. Use the USB setup command to enter a new Wi-Fi password.
- **Tap the home screen** to make Red bounce. **Swipe up or down on a long reply** to read it. The scroll indicator shows how much text is left.
- **Check the status icons** above the avatar. The battery shows the percentage and a lightning bolt while charging. The Wi-Fi icon shows signal strength or a slash when disconnected.

**Voice interrupt beta** enables experimental simultaneous mic and speaker audio in hands-free mode. It defaults to off because that capture path can miss speech. Hold A for an immediate interruption in either mode.

For current information, Red can delegate to web search over Wi-Fi. Include the city in a forecast question, such as “What's tomorrow's weather in Berlin?”

For follow-up questions, Red carries up to four recent question-and-answer pairs into each new standalone voice session. This context clears when the watch restarts.

## Timer

In standalone Wi-Fi mode, hold **A** and say “Set a timer for seven minutes.” Red opens a large HH:MM:SS countdown after confirming the start, an extension, or a status check. Hold **B** to return to the avatar while the timer runs. Ask “How much time is left?”, “Add one minute,” or “Cancel the timer” in a later turn. Red supports one countdown from one second to 24 hours. When it expires, the watch vibrates and plays a chime. The countdown and alert run locally after the voice command succeeds, even if Wi-Fi drops. Keep the watch powered on.

## To-do

- Let the user set a local timezone and use it when answering time-sensitive questions.

## USB demo and typed questions

In PowerShell, from this folder:

```powershell
python -m pip install pyserial websocket-client
python orbit.py --demo
```

- **Left button (A)** — Hold until the green **Listening** screen appears, speak, then release. Red takes up to eight seconds of speech. In offline demo mode it plays back what the mic captured.
- **Right button (B)** — Press to replay the most recent spoken reply.
- **Touchscreen** — Tap the home screen to make Red bounce. Swipe to scroll a long reply.

For live voice over USB, run `python orbit.py --ask-key` and enter an OpenAI API key at the hidden prompt. You can also set `OPENAI_API_KEY` in your terminal session and run `python orbit.py`. In USB mode each button recording opens one short voice session after release. Live replies use an AI-generated voice. Type in the terminal for a text answer from GPT-6 Luna. The terminal shows the full caption and the watch shows the portion that fits its ASCII font.

Use `/quit` or Ctrl+C to close the bridge. If autodetection finds multiple Espressif devices, add `--port COM7` or the current StopWatch port. GPT-Live 1 charges by session duration. [Model and pricing](https://developers.openai.com/api/docs/models/gpt-live-1)

## Build and flash

Commit the changes you want on the watch before running the upload command. PlatformIO requires a clean Git branch and pushes the built revision to the same branch on `origin` before it flashes. If the push fails, the upload stops. A build without the upload target works locally without GitHub access.

```powershell
python -m pip install platformio
python -m platformio run
python -m platformio run --target upload --upload-port COM7
python orbit.py --smoke-test
```

Close the bridge before uploading. If the uploader cannot connect, leave the USB cable attached and hold the watch's power button until the green LED blinks. Release it and check the port with `python -m serial.tools.list_ports -v` before retrying. See the [M5Stack StopWatch documentation](https://docs.m5stack.com/en/core/StopWatch) for download mode and factory firmware recovery.

Hands-free echo cancellation uses the bundled echo and preprocessing code from [SpeexDSP 1.2.1](https://github.com/xiph/speexdsp). Its license is in `lib/speexdsp/COPYING`.

## Restore the original flash

For the watch used in this demo, the pre-upload backup is stored locally at `%LOCALAPPDATA%\OrbitStopWatch\stopwatch-backup.bin`. To restore that same watch, close the bridge and run the following with its current serial port:

```powershell
python "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py" --chip esp32s3 --port COM7 --baud 921600 write_flash 0x0 "$env:LOCALAPPDATA\OrbitStopWatch\stopwatch-backup.bin"
```
