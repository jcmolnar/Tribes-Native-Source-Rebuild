// Attach to a (possibly hung) process, walk every thread's stack with symbols, then detach
// without killing it. x86. Usage: hangsample <pid> [sympath]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#pragma comment(lib, "dbghelp.lib")

static HANDLE g_proc;

static void walkThread(DWORD tid) {
    HANDLE ht = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!ht) return;
    CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(ht, &ctx)) { CloseHandle(ht); return; }
    printf("\n--- thread %lu  eip=0x%08X ---\n", tid, ctx.Eip);
    STACKFRAME64 sf; memset(&sf, 0, sizeof(sf));
    sf.AddrPC.Offset = ctx.Eip;    sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Ebp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Esp; sf.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; i++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, g_proc, ht, &sf, &ctx, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL)) break;
        if (!sf.AddrPC.Offset) break;
        char sb[sizeof(SYMBOL_INFO) + 1024]; SYMBOL_INFO* s = (SYMBOL_INFO*)sb;
        s->SizeOfStruct = sizeof(SYMBOL_INFO); s->MaxNameLen = 1024;
        DWORD64 d = 0; IMAGEHLP_LINE64 ln; ln.SizeOfStruct = sizeof(ln); DWORD ld = 0;
        if (SymFromAddr(g_proc, sf.AddrPC.Offset, &d, s)) {
            if (SymGetLineFromAddr64(g_proc, sf.AddrPC.Offset, &ld, &ln))
                printf("  #%02d %s +0x%llX  (%s:%lu)\n", i, s->Name, d, ln.FileName, ln.LineNumber);
            else
                printf("  #%02d %s +0x%llX\n", i, s->Name, d);
        } else {
            printf("  #%02d 0x%08llX\n", i, sf.AddrPC.Offset);
        }
    }
    CloseHandle(ht);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: hangsample <pid> [sympath]\n"); return 2; }
    DWORD pid = atoi(argv[1]);
    if (!DebugActiveProcess(pid)) { printf("DebugActiveProcess(%lu) failed %lu\n", pid, GetLastError()); return 3; }
    DebugSetProcessKillOnExit(FALSE);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
    DEBUG_EVENT ev; bool walked = false;
    while (WaitForDebugEvent(&ev, 15000)) {
        if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            g_proc = ev.u.CreateProcessInfo.hProcess;
            SymInitialize(g_proc, argc >= 3 ? argv[2] : NULL, FALSE);
            SymLoadModule64(g_proc, ev.u.CreateProcessInfo.hFile, NULL, NULL,
                            (DWORD64)ev.u.CreateProcessInfo.lpBaseOfImage, 0);
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
        } else if (ev.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            SymLoadModule64(g_proc, ev.u.LoadDll.hFile, NULL, NULL, (DWORD64)ev.u.LoadDll.lpBaseOfDll, 0);
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
        } else if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            if (!walked && ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                walked = true;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te; te.dwSize = sizeof(te);
                if (Thread32First(snap, &te)) {
                    do { if (te.th32OwnerProcessID == pid) walkThread(te.th32ThreadID); } while (Thread32Next(snap, &te));
                }
                CloseHandle(snap);
                printf("\n=== done sampling; detaching ===\n"); fflush(stdout);
                DebugActiveProcessStop(pid);
                return 0;
            }
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, DBG_CONTINUE);
    }
    printf("timeout / no break\n");
    DebugActiveProcessStop(pid);
    return 0;
}
