# voice-chat-client

Ragnarok Online **Voice Chat Client DLL** — injects into the RO client to enable in-game proximity voice chat.

Built for use with [rathena-voice-chat](https://github.com/Sitecraft-Admin/rathena-voice-chat) server.

[![Website](https://img.shields.io/badge/Website-sitecraft.in.th-blue?style=for-the-badge)](https://sitecraft.in.th/)
[![Discord](https://img.shields.io/badge/Discord-Join%20Us-5865F2?style=for-the-badge&logo=discord&logoColor=white)](https://discord.com/invite/aTkZw9ZrQ9)
[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/sitecraft)

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

## Memory Offsets

**ACCOUNT_ID** and **CHAR_ID** are read from client memory; **LOGIN_ID1** is optional
(anti-spoof hardening — see below). Position (X/Y/Map) is pushed by the map server via
the voice server — no memory scanning needed for spatial audio.

Edit `src/memory_reader.hpp` to match your client version:

```cpp
// Client 2024-08-22
//constexpr uintptr_t ACCOUNT_ID = 0x116B7EC;
//constexpr uintptr_t CHAR_ID    = 0x116B7F0;
//constexpr uintptr_t LOGIN_ID1  = 0x0116B07C;

// Client 2025-07-16
constexpr uintptr_t ACCOUNT_ID = 0x011FB9A4;
constexpr uintptr_t CHAR_ID    = 0x011FB9A8;
constexpr uintptr_t LOGIN_ID1  = 0x011FB244;  // 0 = anti-spoof disabled
```

Use [`tools/ro-mem-scanner.zip`](tools/ro-mem-scanner.zip) to find offsets for other client versions.

> Run as **Administrator** while logged into a character on a map.

### Anti-spoof hardening (LOGIN_ID1)

By default the voice server verifies a client by its shared secret plus an
`account_id` / `char_id` cross-check against the map server's advisory. Anyone who
learns a victim's AID/CID **and** the shared client secret could still impersonate
them on voice.

Setting `LOGIN_ID1` closes that gap. `login_id1` is a random per-login session key
that only the real game client holds in memory; the map server already forwards the
authoritative value to the voice server. With the offset set, the DLL sends its
`login_id1` on auth and the server rejects any session whose value doesn't match —
so AID/CID + secret alone is no longer enough.

Leaving `LOGIN_ID1 = 0` keeps the previous behaviour (fully backward compatible):
the DLL omits the field and the server stays on the AID/CID-only check.

**Finding the offset:** `login_id1` is **not** in the database (it's regenerated at
each login), so the scanner can't pull it from MySQL like AID/CID. Instead:

1. Set `log_level: 3` in the voice server's `conf/voice_athena.conf`, then log a
   character in and watch the log for `auth_advisory ... l1=<value>`.
2. Run `ro-mem-scanner`, complete the AID/CID/X-Y steps, then enter that `l1` value
   at the **STEP 4** prompt. Relog when asked if more than one candidate remains.
3. Copy the printed `LOGIN_ID1 = 0x...` line into `memory_reader.hpp` and rebuild.

---

## Injection

Recommended: use [`tools/VoiceLinker.zip`](tools/VoiceLinker.zip) to patch the RO client EXE so it loads
`voice-chat.dll` automatically on startup.

**Using VoiceLinker:**
- Extract `VoiceLinker.zip`
- Open `VoiceLinker.exe`
- Select your RO client `.exe`
- Click **Inject**
- Put `voice-chat.dll` next to the patched RO client `.exe`
- Start the RO client normally

VoiceLinker creates a `.bak` backup next to the target EXE. To undo the patch, open VoiceLinker again,
select the same EXE, and click **Remove**.

Alternative: inject `voice-chat.dll` into the RO client process using any DLL injector.

**Important:**
- Inject **after** you are logged into a character on a map (not at the login or character select screen)
- If you use VoiceLinker, place `voice-chat.dll` next to the RO client EXE before launching the game
- The overlay appears automatically once the DLL is loaded and connected
- Press **Scroll Lock** to show/hide the overlay at any time

**Verify it's working:**

| Sign | Meaning |
|------|---------|
| Overlay appears in-game | DLL hooked successfully |
| Overlay gone after Scroll Lock | Press Scroll Lock again to restore |
| No overlay at all | DLL not injected, or injected too early (re-inject after entering a map) |

---

## Build

Open `voice-dll.sln` in Visual Studio 2022 and build `Release | Win32`.

Output:

- `dist\voice-chat.dll`

---

## Compatibility

Tested on **Ragnarok Online client 2024-08-22**.

---

## License

Developed by **TiTaNos** — [sitecraft.in.th](https://sitecraft.in.th/)
