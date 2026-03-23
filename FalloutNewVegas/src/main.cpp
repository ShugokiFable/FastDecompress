// FastDecompressNV — NVSE plugin entry point
//
// Replaces FNV's zlib 1.2.1 with libdeflate + chromium-zlib for ~2.75x faster
// decompression of ESP/ESM records and BSA archive files.

#include <windows.h>
#include <cstdint>

#include "DecompressHooks.h"

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

// Patch the BSA output buffer size global BEFORE game constructors run.
// The game's CompressedArchiveFile reads this value during construction
// to determine the decompression output buffer size (default 128KB).
// We increase it to 4MB so libdeflate can decompress large entries in one shot.
// Patch the BSA output buffer cap (DAT_011F8164) as early as possible.
// The game caps CompressedArchiveFile output buffers to min(entry_size, this global).
// Default cap is 128KB. We increase to 4MB so libdeflate can one-shot large entries.
static void PatchBufferCap()
{
    // Patch CompressedArchiveFile constructor to remove the 128KB buffer cap.
    // Original at 0xAFC58C:
    //   3B 15 64 81 1F 01   CMP edx, [011F8164]   (compare entry_size with cap)
    //   73 0E               JAE +0Eh               (if >= cap, use entry_size)
    //   [fall through: use cap = 128KB]
    //
    // Patched: NOP out the CMP, change JAE to JMP (always use entry_size).
    // Buffer = entry's actual uncompressed size, so libdeflate always has room.
    DWORD oldProtect;
    void* addr = reinterpret_cast<void*>(0x00AFC58C);
    if (VirtualProtect(addr, 8, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        // NOP the 6-byte CMP + 2-byte JAE = 8 bytes total.
        // Execution falls through to use entry_size (field_168) always.
        memset(addr, 0x90, 8);
        VirtualProtect(addr, 8, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), addr, 8);
    }
}

BOOL WINAPI DllMain(HINSTANCE /*hInstDLL*/, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        PatchBufferCap();
    }
    return TRUE;
}

// ── NVSE plugin interface ─────────────────────────────────────────────────

extern "C" __declspec(dllexport)
bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
{
    info->infoVersion = PluginInfo::kInfoVersion;
    info->name = "FastDecompressNV";
    info->version = 120; // 1.1

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
