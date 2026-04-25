# voice-chat-client

Ragnarok Online **Voice Chat Client DLL** — injects into the RO client to enable in-game proximity voice chat.

Built for use with [rathena-voice-chat](https://github.com/Sitecraft-Admin/rathena-voice-chat) server.

[![Website](https://img.shields.io/badge/Website-sitecraft.in.th-blue?style=for-the-badge)](https://sitecraft.in.th/)
[![Discord](https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.com/invite/aTkZw9ZrQ9)

---

## Features

- Proximity voice — hear nearby players automatically
- Push to Talk (PTT) / Open Mic mode
- Party / Guild / Room / Whisper channels
- Direct voice call to any character by name
- Per-player mute & volume control
- Mic & Speaker volume sliders
- Anti-tamper protection

---

## Layout

- `src/` — main DLL source and headers
- `src/extras/` — extra files (not part of build)
- `vendor/` — third-party dependencies (ImGui, Opus, nlohmann/json)
- `dist/` — build output

---

## Configuration

Server IP is set in `src/voice_client.hpp`:

```cpp
std::wstring server_host_ = L"127.0.0.1";
INTERNET_PORT server_port_ = 7000;
```

Change `127.0.0.1` to your voice server IP before building.

---

## Build

Open `voice-dll.sln` in Visual Studio 2022 and build `Release | Win32`, or run:

```powershell
.\build.ps1
```

Output:

- `dist\voice-chat.dll`

---

## License

Developed by **TiTaNos** — [sitecraft.in.th](https://sitecraft.in.th/)
