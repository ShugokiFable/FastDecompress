// FastDecompressNV — NVSE plugin entry point
//
// Replaces FNV's zlib 1.2.1 with libdeflate + chromium-zlib for ~2.75x faster
// decompression of ESP/ESM records and BSA archive files.

#include <windows.h>
#include <cstdint>

#include "DecompressHooks.h"
#include "CrashLogger.h"

// ── Minimal NVSE types ────────────────────────────────────────────────────

typedef uint8_t  UInt8;
typedef uint16_t UInt16;
typedef uint32_t UInt32;

struct PluginInfo {
    enum { kInfoVersion = 1 };
    UInt32      infoVersion;
    const char* name;
    UInt32      version;
};

struct NVSEInterface {
    UInt32 nvseVersion;
    UInt32 runtimeVersion;
    UInt32 editorVersion;
    UInt32 isEditor;
    // remaining fields not needed
};

// ── DLL entry point ───────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE /*hInstDLL*/, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        CrashLogger::Install();
    }
    return TRUE;
}

// ── NVSE plugin interface ─────────────────────────────────────────────────

extern "C" __declspec(dllexport)
bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "FastDecompressNV";
    info->version = 100; // 1.0.0

    // Require xNVSE 6.2.0+
    if (!nvse->isEditor && nvse->nvseVersion < 0x06020000) {
        return false;
    }

    return true;
}

extern "C" __declspec(dllexport)
bool NVSEPlugin_Load(NVSEInterface* nvse)
{
    FastDecompress::Install(nvse->isEditor != 0);
    return true;
}
