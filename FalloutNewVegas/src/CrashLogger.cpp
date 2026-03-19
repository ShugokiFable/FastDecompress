// CrashLogger — Vectored Exception Handler with stack trace via DbgHelp
//
// Catches unhandled access violations, stack overflows, illegal instructions,
// etc. Writes a crash log with registers, exception info, and a symbolicated
// call stack using StackWalk (DbgHelp.dll).

#include "CrashLogger.h"
#include "DecompressHooks.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdio>
#include <ctime>
#include <filesystem>

#pragma comment(lib, "dbghelp.lib")

namespace CrashLogger
{
    static FILE* s_crashFile = nullptr;
    static bool  s_hasCrashed = false;

    static const char* ExceptionCodeToString(DWORD code)
    {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
            case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
            case EXCEPTION_BREAKPOINT:               return "BREAKPOINT";
            case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
            case EXCEPTION_FLT_DENORMAL_OPERAND:     return "FLT_DENORMAL_OPERAND";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INEXACT_RESULT:       return "FLT_INEXACT_RESULT";
            case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
            case EXCEPTION_FLT_OVERFLOW:             return "FLT_OVERFLOW";
            case EXCEPTION_FLT_STACK_CHECK:          return "FLT_STACK_CHECK";
            case EXCEPTION_FLT_UNDERFLOW:            return "FLT_UNDERFLOW";
            case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
            case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
            case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW:             return "INT_OVERFLOW";
            case EXCEPTION_INVALID_DISPOSITION:      return "INVALID_DISPOSITION";
            case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
            case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
            case EXCEPTION_SINGLE_STEP:              return "SINGLE_STEP";
            case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
            default:                                 return "UNKNOWN";
        }
    }

    static void WriteModuleInfo(FILE* f, DWORD addr)
    {
        HMODULE hMod = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(addr), &hMod))
        {
            char modName[MAX_PATH]{};
            GetModuleFileNameA(hMod, modName, MAX_PATH);
            DWORD base = reinterpret_cast<DWORD>(hMod);
            fprintf(f, "  Module: %s (base %08Xh, offset +%08Xh)\n", modName, base, addr - base);
        }
    }

    static void WriteStackTrace(FILE* f, CONTEXT* ctx)
    {
        HANDLE process = GetCurrentProcess();
        HANDLE thread  = GetCurrentThread();

        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);

        STACKFRAME frame{};
        frame.AddrPC.Offset    = ctx->Eip;
        frame.AddrPC.Mode      = AddrModeFlat;
        frame.AddrFrame.Offset = ctx->Ebp;
        frame.AddrFrame.Mode   = AddrModeFlat;
        frame.AddrStack.Offset = ctx->Esp;
        frame.AddrStack.Mode   = AddrModeFlat;

        fprintf(f, "\n=== Stack Trace ===\n");

        alignas(IMAGEHLP_SYMBOL) char symbolBuf[sizeof(IMAGEHLP_SYMBOL) + 512];

        for (int i = 0; i < 128; i++) {
            if (!StackWalk(IMAGE_FILE_MACHINE_I386, process, thread, &frame, ctx,
                           nullptr, SymFunctionTableAccess, SymGetModuleBase, nullptr))
                break;

            if (frame.AddrPC.Offset == 0)
                break;

            DWORD pc = static_cast<DWORD>(frame.AddrPC.Offset);

            // Module name + offset
            HMODULE hMod = nullptr;
            char modName[MAX_PATH] = "???";
            DWORD modBase = 0;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(pc), &hMod))
            {
                GetModuleFileNameA(hMod, modName, MAX_PATH);
                modBase = reinterpret_cast<DWORD>(hMod);
                // Just filename
                const char* slash = strrchr(modName, '\\');
                if (slash) memmove(modName, slash + 1, strlen(slash));
            }

            // Symbol name
            auto* sym = reinterpret_cast<IMAGEHLP_SYMBOL*>(symbolBuf);
            memset(sym, 0, sizeof(symbolBuf));
            sym->SizeOfStruct  = sizeof(IMAGEHLP_SYMBOL);
            sym->MaxNameLength = 512;

            DWORD symDisp = 0;
            const char* symName = "???";
            if (SymGetSymFromAddr(process, pc, &symDisp, sym)) {
                symName = sym->Name;
            }

            // Source file + line
            IMAGEHLP_LINE line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;

            if (SymGetLineFromAddr(process, pc, &lineDisp, &line)) {
                fprintf(f, "  [%02d] %08Xh  %s+%04Xh  %s  (%s:%lu)\n",
                    i, pc, modName, pc - modBase, symName, line.FileName, line.LineNumber);
            } else {
                fprintf(f, "  [%02d] %08Xh  %s+%04Xh  %s\n",
                    i, pc, modName, pc - modBase, symName);
            }
        }

        SymCleanup(process);
    }

    static LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ep)
    {
        DWORD code = ep->ExceptionRecord->ExceptionCode;

        // Skip non-fatal exceptions (C++ exceptions, debug events, etc.)
        if (code == 0xE06D7363   // C++ exception (MSVC)
            || code == 0x406D1388 // SetThreadName
            || code == 0x40010006 // OutputDebugStringA
            || code == 0x40010007 // OutputDebugStringW
            || code == EXCEPTION_BREAKPOINT
            || code == EXCEPTION_SINGLE_STEP
            || (code & 0xF0000000) == 0x40000000) // All informational exceptions
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Prevent recursive crash logging
        if (s_hasCrashed) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        s_hasCrashed = true;

        // Open crash log next to our DLL
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&VectoredHandler), &hm);
        char dllPath[MAX_PATH]{};
        GetModuleFileNameA(hm, dllPath, MAX_PATH);
        std::filesystem::path crashPath = std::filesystem::path(dllPath).replace_extension(".crash.log");
        s_crashFile = fopen(crashPath.string().c_str(), "w");

        if (!s_crashFile) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        FILE* f = s_crashFile;

        // Timestamp
        time_t now = time(nullptr);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", localtime(&now));

        fprintf(f, "=== FastDecompressNV Crash Log ===\n");
        fprintf(f, "Version: %s\n", FastDecompress::kVersion);
        fprintf(f, "Time: %s\n", timeBuf);

        // Exception info
        fprintf(f, "\n=== Exception ===\n");
        fprintf(f, "  Code: %08Xh (%s)\n", code, ExceptionCodeToString(code));
        fprintf(f, "  Address: %08Xh\n",
            static_cast<DWORD>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)));

        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            const char* op = ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "reading" : "writing";
            fprintf(f, "  Attempted %s address: %08Xh\n", op,
                static_cast<DWORD>(ep->ExceptionRecord->ExceptionInformation[1]));
        }

        WriteModuleInfo(f, static_cast<DWORD>(
            reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)));

        // Registers
        CONTEXT* ctx = ep->ContextRecord;
        fprintf(f, "\n=== Registers ===\n");
        fprintf(f, "  EAX=%08Xh  EBX=%08Xh  ECX=%08Xh  EDX=%08Xh\n",
            ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
        fprintf(f, "  ESI=%08Xh  EDI=%08Xh  EBP=%08Xh  ESP=%08Xh\n",
            ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
        fprintf(f, "  EIP=%08Xh  EFLAGS=%08Xh\n", ctx->Eip, ctx->EFlags);

        // Stack trace
        WriteStackTrace(f, ctx);

        // Loaded modules
        fprintf(f, "\n=== Loaded Modules ===\n");
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me{};
            me.dwSize = sizeof(me);
            if (Module32First(snap, &me)) {
                do {
                    fprintf(f, "  %08Xh - %08Xh  %ls\n",
                        reinterpret_cast<DWORD>(me.modBaseAddr),
                        reinterpret_cast<DWORD>(me.modBaseAddr) + me.modBaseSize,
                        me.szModule);
                } while (Module32Next(snap, &me));
            }
            CloseHandle(snap);
        }

        fflush(f);
        fclose(f);
        s_crashFile = nullptr;

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void Install()
    {
        AddVectoredExceptionHandler(0, VectoredHandler);
    }
}
