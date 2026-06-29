# Tribes Native Source Rebuild

A **working native Windows (MSVC / Win32) client** for *Starsiege: Tribes* (1998),
rebuilt from the community-reverse-engineered **Darkstar** engine source. It
connects to live, unmodified Tribes servers — including the **Kingdom of Kronos**
RPG server — and renders in hardware **OpenGL**.

This is a from-source rebuild: zero bytes of any shipped retail binary are used.
Every line is compiled from the open engine source tree, then patched where the
1998 code needed fixing to build under a modern toolchain and to survive real
wire data from today's servers.

## What works

- Compiles cleanly with **MSVC v143** (Visual Studio 2022 / VS18 Build Tools), Win32 / Release.
- Connects to live Kronos servers, loads in, spawns, and renders the 3D world in
  **hardware OpenGL** (verified on an RTX 3080 / GL 4.6).
- Server browser, server **INFO** dialog, and player list.
- Window auto-sizes to the render surface; the cursor tracks the OS pointer at any window size.
- **ScriptGL HUD** — the team5150 ScriptGL script-draw interface (absent from the
  open source) reimplemented in `engine/SimGui/code/scriptGL.cpp`, so the Kronos
  vhud HUD renders: panels, colors, transparency, HP/XP bars, and a chat composer
  with real keyboard text input.

See **[HANDOFF.md](HANDOFF.md)** for build / run / debug commands, the full list of
engine fixes (grep `NATIVE-PORT`), and the two known remaining gaps (the KHudOn
server-data handshake and TrueType HUD fonts).

## Build

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  ".\TribesNative.vcxproj" /p:Configuration=Release /p:Platform=Win32 `
  "/p:SolutionDir=$PWD\" /m /nologo /v:minimal
# Output: bin\Release\TribesNative\TribesNative.exe
```

Game assets (`.vol` archives, scripts, configs) are **not** included — install
them from a copy of the freeware game (Tribes has been free since 2015) and point
the working directory at that install.

## Source layout

- `engine/` — the Darkstar engine (Core, Ml, Dgfx, DNet, Sim, SimGui, Ts3, Terrain, Interior, …)
- `program/` — the Tribes/Fear game module
- `inc/` — shared headers
- `TribesNative.vcxproj` — the MSVC build project
- `_dbg/` — small standalone crash/hang debuggers used during the port

## Lineage & credits

This rebuild stands on the community reverse-engineering work that recovered the
Tribes/Darkstar engine source:

- **[leechristensen/TribesRebirth](https://github.com/leechristensen/TribesRebirth)**
  — the upstream this project takes its bearings from (itself a fork of
  **[AltimorTASDK/TribesRebirth](https://github.com/AltimorTASDK/TribesRebirth)**),
  a Darkstar source tree set up to rebuild with the original Borland/TASM tools to
  assist reverse engineering (IDA / Ghidra / BinDiff signature matching).
- The broader **TribesSource** community engine tree, from which this restructured
  `engine/ + program/ + inc/` layout and the MSVC build are derived.

All credit for recovering the original engine goes to those projects and the
Tribes RE community. This repository's contribution is the modern-MSVC build, the
OpenGL/native client fixes, and the ScriptGL HUD reimplementation.

A sibling project compiles the same engine to **WebAssembly** to run the client in
a browser; this native build began as a debugging aid for that port and grew into
a standalone deliverable.

## Status / legality

The original Tribes game is freeware. The engine source is community
reverse-engineered; treat it accordingly. This repository is shared for
preservation, research, and interoperability with existing Tribes servers.
