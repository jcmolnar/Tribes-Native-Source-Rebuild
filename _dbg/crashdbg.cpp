// Minimal x86 crash debugger: launch a process, on a fatal exception print a
// symbolized stack (with source file:line via the matching PDB), then terminate.
// Built x86 to match TribesNative.exe. Usage: crashdbg <exe> [cwd] [sympath]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#pragma comment(lib, "dbghelp.lib")

static HANDLE g_proc;

static void walk(DWORD tid, EXCEPTION_RECORD* er, int firstChance) {
    HANDLE ht = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_FULL;
    GetThreadContext(ht, &ctx);
    printf("\n=== EXCEPTION 0x%08X at 0x%p  firstChance=%d tid=%lu ===\n",
           er->ExceptionCode, er->ExceptionAddress, firstChance, tid);
    if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2)
        printf("    access violation %s 0x%p\n",
               er->ExceptionInformation[0] ? "writing" : "reading",
               (void*)er->ExceptionInformation[1]);
    STACKFRAME64 sf; memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset = ctx.Eip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp; sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 60; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, g_proc, ht, &sf, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        if (!sf.AddrPC.Offset) break;
        char sb[sizeof(SYMBOL_INFO) + 1024]; SYMBOL_INFO* s = (SYMBOL_INFO*)sb;
        s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 1024;
        DWORD64 disp = 0; IMAGEHLP_LINE64 ln; ln.SizeOfStruct = sizeof(ln); DWORD ld = 0;
        if (SymFromAddr(g_proc, sf.AddrPC.Offset, &disp, s)) {
            if (SymGetLineFromAddr64(g_proc, sf.AddrPC.Offset, &ld, &ln))
                printf("  #%02d %s +0x%llX   (%s:%lu)\n", i, s->Name, disp, ln.FileName, ln.LineNumber);
            else
                printf("  #%02d %s +0x%llX\n", i, s->Name, disp);
        } else {
            printf("  #%02d 0x%08llX\n", i, sf.AddrPC.Offset);
        }
    }
    CloseHandle(ht);
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: crashdbg <exe> [cwd] [sympath]\n"); return 2; }
    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    char cmd[2048]; _snprintf(cmd, sizeof(cmd), "\"%s\"", argv[1]);
    const char* cwd = argc >= 3 ? argv[2] : NULL;
    if (!CreateProcessA(argv[1], cmd, NULL, NULL, FALSE, DEBUG_ONLY_THIS_PROCESS, NULL, cwd, &si, &pi)) {
        printf("CreateProcess failed %lu\n", GetLastError()); return 3;
    }
    g_proc = pi.hProcess;
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(pi.hProcess, argc >= 4 ? argv[3] : NULL, FALSE);
    printf("launched %s (cwd=%s)\n", argv[1], cwd ? cwd : "(inherit)"); fflush(stdout);
    DEBUG_EVENT ev; int crashes = 0; int cppwalks = 0; DWORD start = GetTickCount();
    while (WaitForDebugEvent(&ev, 610000)) {
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            DWORD64 base = (DWORD64)ev.u.CreateProcessInfo.lpBaseOfImage;
            DWORD64 r = SymLoadModule64(pi.hProcess, ev.u.CreateProcessInfo.hFile, "TribesNativeDbg.exe", NULL, base, 0);
            IMAGEHLP_MODULE64 mi; memset(&mi, 0, sizeof(mi)); mi.SizeOfStruct = sizeof(mi);
            BOOL ok = SymGetModuleInfo64(pi.hProcess, base, &mi);
            printf("[sym] exe base=0x%llX SymLoadModule=0x%llX symType=%d pdb=%s\n",
                   base, r, ok ? (int)mi.SymType : -1, ok ? mi.LoadedPdbName : "(none)"); fflush(stdout);
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break; }
        case LOAD_DLL_DEBUG_EVENT:
            SymLoadModule64(pi.hProcess, ev.u.LoadDll.hFile, NULL, NULL,
                            (DWORD64)ev.u.LoadDll.lpBaseOfDll, 0);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            break;
        case EXCEPTION_DEBUG_EVENT: {
            DWORD c = ev.u.Exception.ExceptionRecord.ExceptionCode;
            if (c == EXCEPTION_BREAKPOINT || c == 0x4000001F) { cont = DBG_CONTINUE; break; }
            if (c == 0x406D1388) { cont = DBG_EXCEPTION_NOT_HANDLED; break; }   // thread-name marker, ignore
            if (c == 0xE06D7363) {   // MSVC C++ throw - walk it so we see the throw site
                printf("\n=== C++ exception (0xE06D7363) firstChance=%d tid=%u ===\n", ev.u.Exception.dwFirstChance, ev.dwThreadId); fflush(stdout);
                if (cppwalks < 6) { walk(ev.dwThreadId, &ev.u.Exception.ExceptionRecord, ev.u.Exception.dwFirstChance); cppwalks++; }
                if (!ev.u.Exception.dwFirstChance) { printf("\n=== unhandled C++ exception, terminating ===\n"); fflush(stdout); TerminateProcess(pi.hProcess, 1); }
                cont = DBG_EXCEPTION_NOT_HANDLED; break;
            }
            walk(ev.dwThreadId, &ev.u.Exception.ExceptionRecord, ev.u.Exception.dwFirstChance);
            crashes++;
            if (!ev.u.Exception.dwFirstChance || crashes > 3) {
                printf("\n=== fatal, terminating ===\n"); fflush(stdout);
                TerminateProcess(pi.hProcess, 1);
            }
            cont = DBG_EXCEPTION_NOT_HANDLED;
            break; }
        case OUTPUT_DEBUG_STRING_EVENT: {
            char buf[512]; SIZE_T rd = 0;
            if (ev.u.DebugString.nDebugStringLength && ev.u.DebugString.lpDebugStringData) {
                SIZE_T n = ev.u.DebugString.nDebugStringLength; if (n > 511) n = 511;
                if (ReadProcessMemory(pi.hProcess, ev.u.DebugString.lpDebugStringData, buf, n, &rd)) {
                    buf[rd] = 0; printf("[ODS] %s", buf); fflush(stdout);
                }
            }
            break; }
        case EXIT_PROCESS_DEBUG_EVENT:
            printf("\n=== process exited, code %lu ===\n", ev.u.ExitProcess.dwExitCode);
            fflush(stdout); return 0;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
        if (GetTickCount() - start > 600000) {
            printf("\n=== 600s elapsed, killing (no crash yet => it may have launched OK) ===\n");
            fflush(stdout); TerminateProcess(pi.hProcess, 1); break;
        }
    }
    printf("\n=== debug loop ended ===\n"); return 0;
}
