# Auto-Mute Audio on Windows

This project is a lightweight Windows program that automatically mutes audio sessions for specific devices (e.g., microphones, headsets) to prevent echo or unwanted sound during screen sharing or other scenarios. It runs silently in the background and requires minimal configuration.

## Features

- Automatically mutes sessions for selected audio devices
- **Supports muting multiple applications at once** (e.g., Discord, Zoom, and Teams simultaneously)
- Works for dynamically created audio sessions (e.g., when starting a screen share or launching an app)
- Runs in the background with minimal CPU and memory usage
- System tray icon with quick controls: Pause/Resume and Exit
- Configurable via config.json
- **Built-in elevated autostart support** via Task Scheduler when `"autostart": true`
- Scheduled task policy is automatically maintained:
  - No execution time limit (won't stop after 3 days)
  - Stop existing instance on new start
  - Allowed on battery power (start + continue)
- Custom tray icon support via tray.ico in the executable folder
- Tested on Windows 10/11
- Lightweight and suitable for autostart

## Motivation

During meetings, screen sharing, or audio recording, certain devices (e.g., microphones or headsets) can introduce echo or feedback. Manually muting audio sessions for each device and application is tedious. This program automates the process:

1. Mutes audio output for selected devices for one or more specified applications  
2. Ensures that new audio sessions are automatically muted

The program periodically checks all audio sessions and enforces mute on matching devices for the configured target applications.

## Installation

1. Download this repository as zip and unpack.

2. Optional: Compile the program (gcc - gnu compiler collection necessary or use the pre-compiled AutoMuteApp version in this repository if your do not want to compile and just start with it):
   `gcc src/*.c -Iinclude -lole32 -luuid -lpropsys -lpsapi -ladvapi32 -mwindows -o AutoMuteApp.exe`

  Or use the included build script:

  `compile.bat`

  Notes for compile.bat:
  - Requests UAC automatically if started from a non-admin terminal
  - Stops running AutoMuteApp instances before compile
  - Fails fast with a clear message if an elevated running instance cannot be terminated

3. Place config.json in the same folder as the executable.
4. Edit config.json and place your preferences in the required fields (the program is case sensitive and only needs as many words as necessary to distinguish it from other devices, so not the whole name is required).
5. Optional: Set `"autostart": true` in config.json to automatically start the program at Windows login. Alternatively, you can press **Win + R**, type `shell:startup`, and place a shortcut there.

  Recommended: use `"autostart": true` and run the app once. If needed, accept the one-time UAC prompt so the scheduled task can be created/updated.

## Configuration

Edit config.json to adjust:

- Target applications: a list of executables for which audio sessions should be muted (e.g., Discord.exe, Zoom.exe, Teams.exe – you can specify as many as you need)
- Devices: audio devices to mute (partial names are supported)
- Polling interval: how frequently the program checks audio sessions (in milliseconds)
- Autostart: set to `true` to register the program to start automatically at Windows login, or `false` to remove autostart

Example config.json:
```json
{
  "poll_interval_ms": 2000,
  "target_applications": [
    "Discord.exe",
    "Zoom.exe",
    "Teams.exe"
  ],
  "autostart": true,
  "devices": [
    { "name_contains": "Aux" },
    { "name_contains": "Headset" }
  ]
}
```

> **Backward compatibility:** The old `"target_application": "Discord.exe"` format (single string) is still supported. If both `target_applications` and `target_application` are present, the array takes precedence.

## How to fix/reverse if problems occured
1. Make sure the **AutoMuteApp.exe** is **not** running anymore in the background, using the task manager to eventually close the background task.
2. Press **Win + R** and search for `sndvol`.
3. Look for your devices and unmute the corresponding tasks.
4. To disable autostart, set `"autostart": false` in config.json and run the program once, or delete the **AutoMuteApp** task from Task Scheduler.

## Tray Controls

- Right-click tray icon: open menu
- Pause/Resume: temporarily stop/start background mute enforcement
- Exit: cleanly stop background worker and close app
- Hover tooltip: displays app name
- Tray menu scales with system DPI settings

## Autostart (Task Scheduler)

When `"autostart": true`, the app configures a Scheduled Task named **AutoMuteApp** with:

- Trigger: At logon
- Run level: Highest
- MultipleInstancesPolicy: StopExisting
- ExecutionTimeLimit: PT0S (no limit)
- DisallowStartIfOnBatteries: false
- StopIfGoingOnBatteries: false

Quick verification command:

`schtasks /Query /TN "AutoMuteApp" /XML | findstr /I "ExecutionTimeLimit MultipleInstancesPolicy DisallowStartIfOnBatteries StopIfGoingOnBatteries"`

Expected output values:

- ExecutionTimeLimit: PT0S
- MultipleInstancesPolicy: StopExisting
- DisallowStartIfOnBatteries: false
- StopIfGoingOnBatteries: false

## Custom Tray Icon

- Place a file named `tray.ico` in the same folder as `AutoMuteApp.exe`
- The app will use it automatically for the tray icon
- If missing, it falls back to the default Windows application icon

## How it Works

1. On start, the program reads config.json.
2. If `"autostart"` is `true`, creates/updates the Scheduled Task startup entry (with required policies). If `false`, removes the task.
3. Creates a tray icon and hidden message window.
4. Runs a background thread that periodically enumerates all active audio endpoints.
5. For each endpoint, checks for devices that match the configured names.
6. For matching devices, enumerates all audio sessions.
7. If the session belongs to any of the target applications, it enforces mute.

This loop ensures that even sessions created dynamically (e.g., launching a new call or screen share) are immediately muted.

## Known Limitations

- Tested on Windows 10/11 only
- Audio muting is one-way; the program does not restore audio
- Behavior may vary with future Windows or application updates
- Requires at least one target application to be running
