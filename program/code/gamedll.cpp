// WASM-PORT: these are the Win32 game-DLL entry-point exports (Tribes loaded program/ as a
// DLL whose `open`/`close` exports the engine's SimDLLObject loader resolved via
// GetProcAddress). On wasm there is no DLL loader (simDLLObject is stubbed inert), so nothing
// calls these — and as plain `extern "C" void open()/close()` they SHADOW libc's
// open(const char*,int,...)/close(int). When the shim FindFirstFileA -> opendir -> open()
// path runs (ResourceManager::setSearchPath), the wrong-signature bind traps the wasm module
// ("unreachable"). Compile them out under emscripten so the real libc open/close win.
#ifndef __EMSCRIPTEN__

extern "C" void __export open()
{
}

extern "C" void __export close()
{
}

#endif
