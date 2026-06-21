# Tribes Native Build — Handoff

A standalone **native MSVC/Win32** Tribes client, rebuilt from the community
Darkstar engine source (TribesSource), that connects to live **Kingdom of Kronos**
RPG servers and renders in hardware **OpenGL**. This is its own git repo, kept
**separate from the WASM browser-based tree** (`C:\Users\Joe\Desktop\Tribes
Browser Based`) — never edit the WASM tree for native work and vice-versa.

## Build / run / debug

```
# Build (Release/Win32, MSVC v143 via VS18 BuildTools):
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  ".\TribesNative.vcxproj" /p:Configuration=Release /p:Platform=Win32 `
  "/p:SolutionDir=<this repo>\" /m /nologo /v:minimal
# Output: bin\Release\TribesNative\TribesNative.exe
# Deploy:  copy to C:\Dynamix\Tribes\TribesNative.exe  (assets live in C:\Dynamix\Tribes)
```

- **Debuggers** in `_dbg/` (built x86 via vcvarsall x86 + `cl /EHsc /Zi crashdbg.cpp /link dbghelp.lib`):
  - `crashdbg.exe <exe> <cwd> <symdir>` — CreateProcess DEBUG_ONLY_THIS_PROCESS + StackWalk64 + DbgHelp; symbolizes access violations AND `0xE06D7363` C++ throws; 10-min window. Run a **renamed copy** `TribesNativeDbg.exe` under it (dodges AppCompat err-740).
  - `hangsample.exe` — DebugActiveProcess + thread walk, detaches without killing.
  - Set `$env:_NO_DEBUG_HEAP = "1"` before launching so heap-corruption Heisenbugs reproduce under the debugger. Build is **/DEBUG:FULL** (FASTLINK PDBs don't symbolize).
- **View the window** (computer-use): `request_access ["tribesnative.exe"]` → `open_application "Tribesnative"` → `screenshot`. NOTE: the in-game Video tab **mislabels** the renderer as Software; it is really OpenGL (the `ogldbg` trace showed `renderer=NVIDIA RTX 3080 / version=4.6.0`).

## What works

Connects to live Kronos, loads in, spawns, renders the 3D world in **hardware OpenGL**.
Window auto-sizes to the render surface; cursor tracks the OS pointer at any size;
server browser, INFO dialog, player list all work. The Kronos **ScriptGL HUD renders**
(panels, colors, transparency, HP bars, XP bar).

Key engine fixes over stock TribesSource (grep `NATIVE-PORT`):
- `gwCanvas.cpp` initSurfaces — boot the **OpenGL** device. (Do NOT add the
  fullscreenDevice=NULL / block-Software-revert variants — they NULL fullscreenDevice
  and crash boot; the active windowed device stays OpenGL regardless.)
- `simGuiCanvas.cpp` — window auto-resize to surface; `processMouseEvent` window→GUI
  cursor scaling; the **ScriptGL render hook** (see below).
- `FearGuiServerInfo.cpp` — load its own fonts + render via `Parent::onRender`
  (was `Grandparent`, which skipped the cell render → empty list).
- `FearGuiUnivButton.h` `setMode` — implemented (was a `throw std::exception()` stub
  that crashed the server INFO dialog).
- console cmds: `simInputPlugin.cpp` pushActionMap/popActionMap; `console.cpp`
  isFunction; `simGuiPlugin.cpp` Control::getExtent/getPosition/setPosition +
  Hudbot::addReplacement stub.
- wire/load crash guards (createDataBlock, RemoteCreate, Player mount, Lightning,
  PlayerManager clientLink cycle, gOGLFn ClearScreen, gOGLSfc _setPalette/FormatMessage).

## ScriptGL HUD — implemented (`engine/SimGui/code/scriptGL.cpp`)

The Kronos HUD is the **team5150 ScriptGL** addon, which was a native engine patch on
the real (Borland) client and is **not in TribesSource**. Reimplemented here:

- **gl\* console commands**: `glColor4ub(r,g,b,a)` bytes 0-255 (NOT `glColor`!) +
  `glRectangle(x,y,w,h)` drawn via **raw GL** (glPushAttrib / glColor4ub /
  glBegin(GL_QUADS) / glVertex2i, GL_BLEND on) so HUD rects get arbitrary RGBA — the
  engine's 2D path is palette-indexed. `glSetFont`/`glDrawString` via the engine
  `.pft` `drawText_p`; `glGetStringDimensions`; inert `glDisable/glEnable/glBlendFunc`.
- **Render hook** `ScriptGL_renderHook(SimObject*, GFXSurface*)`: wired into
  `Canvas::render` **after** each content control's `onRender` (so the HUD overlays
  the 3D/GUI, under the cursor). If `ScriptGL::<controlName>::onPreDraw` exists
  (`gEvalState.frames[0]->lookup`), calls onPreDraw/onPostDraw with `"<w> <h>"`. The
  in-game control is **`PlayGui`**.

Kronos client scripts (the addon — under `C:\Dynamix\Tribes\config`, NOT this repo):
`scriptgl2.cs` (the vhud system), `Presto/KronosHUD.cs` (HUD elements + the
`remoteKronosHUD` data handler), `Presto/KronosChat.cs`, `KronosNPC.cs`, `KronosShop.cs`.

## TODO — two remaining gaps

1. **Info panels are empty = the KHudOn server-data handshake isn't completing.**
   `kronos::info_onrender` (KronosHUD.cs:589) returns early on `!$KH::hasData`.
   `$KH::*` (Lv/Class/Gold/Mana) is set by `remoteKronosHUD(server,hp,...,lvl,remort)`
   (KronosHUD.cs:22) — a **server→client `remoteEval` push** the server **gates on the
   KHudOn handshake** (KronosShop.cs:9). `remoteEval` is registered (simGame.cpp:890 +
   RemoteEval handler ~662); our client receives other server remote cmds
   (`remoteClient::JoinMessages`). KHudOn is only re-affirmed *inside* remoteKronosHUD
   (line 50), so it relies on the server's bootstrap push arriving.
   **NEXT:** add a temporary trace to the server→client remote-command receive path
   (the CSDelegate path that evaluates received remote cmds) → reconnect → confirm
   whether `remoteKronosHUD` actually arrives. Then fix the **send** (have the client
   send the initial `remoteEval(2048, KHudOn)` on PlayGui open) vs the **receive**.

2. **TrueType fonts.** `glSetFont` currently loads a fixed `sf_white_7.pft` and ignores
   the requested name/pixel-height/glow, so HUD text is mis-sized. ScriptGL fonts are
   TrueType (KronosHUD.cs: `vhud::render_text(pos,"Verdana",size,$GLEX_SMOOTH,text)`).
   Proper fix = GDI HFONT → render text to a GL texture (alpha from coverage, color =
   the current glColor4ub) in glSetFont/glDrawString/glGetStringDimensions.

## Commits
- `71bcd54` working native client (connect/spawn/OpenGL)
- `aaff9b1` strip temp debug logging
- `5e22e94` ScriptGL drawing interface — HUD renders (panels, colors, bars)
