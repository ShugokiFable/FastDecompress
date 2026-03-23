#include "DecompressHooks.h"

#include <libdeflate.h>
#include <zlib.h>       // chromium-zlib — SSE2 SIMD streaming inflate for BSA
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <filesystem>

namespace FastDecompress
{
    // ========================================================================
    // INI-driven config
    // ========================================================================

    static bool s_enableStats = false;
    static bool s_baselineMode = false;

    static void LoadINI() {
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LoadINI), &hm);
        char dllPath[MAX_PATH]{};
        GetModuleFileNameA(hm, dllPath, MAX_PATH);
        std::filesystem::path iniPath = std::filesystem::path(dllPath).replace_extension(".ini");
        s_enableStats = GetPrivateProfileIntA("General", "bEnableLogging", 0, iniPath.string().c_str()) != 0;
        s_baselineMode = GetPrivateProfileIntA("General", "bBaselineMode", 0, iniPath.string().c_str()) != 0;
    }

    // ========================================================================
    // Logging
    // ========================================================================

    static FILE* s_logFile = nullptr;

    static void LogOpen() {
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LogOpen), &hm);
        char dllPath[MAX_PATH]{};
        GetModuleFileNameA(hm, dllPath, MAX_PATH);
        std::filesystem::path logPath = std::filesystem::path(dllPath).replace_extension(".log");
        s_logFile = fopen(logPath.string().c_str(), "w");
    }

    static void LogInfo(const char* fmt, ...) {
        if (!s_logFile) return;
        va_list args;
        va_start(args, fmt);
        fprintf(s_logFile, "[FastDecompressNV] ");
        vfprintf(s_logFile, fmt, args);
        fprintf(s_logFile, "\n");
        fflush(s_logFile);
        va_end(args);
    }

    // ========================================================================
    // QPC timing & prefetch
    // ========================================================================

    struct Stats {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> bytesIn{0};
        std::atomic<uint64_t> bytesOut{0};
        std::atomic<int64_t>  ticksTotal{0};
    };

    static Stats s_formStats;     // libdeflate (TESFile forms + BSA one-shot)
    static Stats s_streamStats;   // chromium-zlib (BSA streaming fallback)
    static int64_t s_qpcFreq = 0;
    static int64_t s_installTime = 0;

    static int64_t QpcNow() {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }

    static double TicksToMs(int64_t ticks) {
        return s_qpcFreq > 0 ? (ticks * 1000.0 / s_qpcFreq) : 0.0;
    }

    // Wall clock tracking
    static std::atomic<int64_t> s_firstCallQpc{0};
    static std::atomic<int64_t> s_lastCallQpc{0};

    static int64_t TrackCallTime() {
        auto now = QpcNow();
        int64_t expected = 0;
        s_firstCallQpc.compare_exchange_strong(expected, now, std::memory_order_relaxed);
        s_lastCallQpc.store(now, std::memory_order_relaxed);
        return now;
    }

    // Snapshot for delta computation
    struct Snapshot {
        uint64_t formCalls, formIn, formOut;
        int64_t  formTicks;
        uint64_t streamCalls, streamIn, streamOut;
        int64_t  streamTicks;
        double   elapsed;
    };
    static Snapshot s_prev{};

    static std::atomic<bool> s_stopLogging{false};

    // BSA libdeflate one-shot hit/miss counters (declared early for LogStats)
    static std::atomic<uint64_t> s_libdeflateBsaHits{0};
    static std::atomic<uint64_t> s_libdeflateBsaMisses{0};

    static void LogStats()
    {
        auto elapsed = TicksToMs(QpcNow() - s_installTime);

        auto fc  = s_formStats.calls.load();
        auto fbi = s_formStats.bytesIn.load();
        auto fbo = s_formStats.bytesOut.load();
        auto ftRaw = s_formStats.ticksTotal.load();
        auto ft  = TicksToMs(ftRaw);

        auto sc  = s_streamStats.calls.load();
        auto sbi = s_streamStats.bytesIn.load();
        auto sbo = s_streamStats.bytesOut.load();
        auto stRaw = s_streamStats.ticksTotal.load();
        auto st  = TicksToMs(stRaw);

        LogInfo("=== FastDecompressNV Stats (%.1fs since install) ===", elapsed / 1000.0);
        LogInfo("  libdeflate (form): %llu calls, %.1f MB in -> %.1f MB out, %.1f ms total",
            fc, fbi / (1024.0 * 1024.0), fbo / (1024.0 * 1024.0), ft);
        LogInfo("  chromium (stream): %llu calls, %.1f MB in -> %.1f MB out, %.1f ms total",
            sc, sbi / (1024.0 * 1024.0), sbo / (1024.0 * 1024.0), st);

        double totalMs = ft + st;
        double totalMB = (fbo + sbo) / (1024.0 * 1024.0);
        LogInfo("  Combined: %.1f ms decompression time, %.1f MB decompressed", totalMs, totalMB);

        if (totalMB > 0) {
            LogInfo("  Throughput: %.0f MB/s", totalMs > 0 ? (totalMB / (totalMs / 1000.0)) : 0);
        }

        auto bsaHits = s_libdeflateBsaHits.load();
        auto bsaMisses = s_libdeflateBsaMisses.load();
        if (bsaHits + bsaMisses > 0) {
            LogInfo("  BSA libdeflate: %llu hits, %llu misses (%.0f%% one-shot)",
                bsaHits, bsaMisses,
                (bsaHits * 100.0) / (bsaHits + bsaMisses));
        }

        // Wall clock loading phase
        auto first = s_firstCallQpc.load();
        auto last  = s_lastCallQpc.load();
        if (first > 0 && last > first) {
            double wallMs = TicksToMs(last - first);
            LogInfo("  wall clock: first->last call = %.1f ms  (CPU/wall = %.1f%%)",
                wallMs, totalMs > 0 ? (totalMs / wallMs * 100.0) : 0.0);
        }

        // Delta since last log
        if (s_prev.elapsed > 0) {
            double dt = elapsed - s_prev.elapsed;
            uint64_t dCalls = (fc - s_prev.formCalls) + (sc - s_prev.streamCalls);
            double dMB = ((fbo - s_prev.formOut) + (sbo - s_prev.streamOut)) / (1024.0 * 1024.0);
            double dCpuMs = TicksToMs((ftRaw - s_prev.formTicks) + (stRaw - s_prev.streamTicks));
            if (dCalls > 0) {
                LogInfo("  delta (%.0fs): %llu calls  %.1f MB  %.1f ms CPU  (%.1f ms CPU/min)",
                    dt / 1000.0, dCalls, dMB, dCpuMs, dt > 0 ? (dCpuMs / (dt / 60000.0)) : 0.0);
            } else {
                LogInfo("  delta (%.0fs): idle (no decompression activity)", dt / 1000.0);
            }
        }

        s_prev = { fc, fbi, fbo, ftRaw, sc, sbi, sbo, stRaw, elapsed };
    }

    // ========================================================================
    // Game's z_stream layout (32-bit, zlib 1.2.1)
    //
    //   offset  field
    //   0x00    next_in      (const uint8_t*)
    //   0x04    avail_in     (uint32_t)
    //   0x08    total_in     (uint32_t)
    //   0x0C    next_out     (uint8_t*)
    //   0x10    avail_out    (uint32_t)
    //   0x14    total_out    (uint32_t)
    //   0x18    msg          (const char*)
    //   0x1C    state        (void*)
    //   0x20    zalloc       (alloc_func)
    //   0x24    zfree        (free_func)
    //   0x28    opaque       (void*)
    //   0x2C    data_type    (int)
    //   0x30    adler        (uint32_t)
    //   0x34    reserved     (uint32_t)
    //   total = 0x38 = 56 bytes
    // ========================================================================

    struct z_stream_game {
        const uint8_t*  next_in;    // 0x00
        uint32_t        avail_in;   // 0x04
        uint32_t        total_in;   // 0x08
        uint8_t*        next_out;   // 0x0C
        uint32_t        avail_out;  // 0x10
        uint32_t        total_out;  // 0x14
        const char*     msg;        // 0x18
        void*           state;      // 0x1C
        void*           zalloc;     // 0x20
        void*           zfree;      // 0x24
        void*           opaque;     // 0x28
        int             data_type;  // 0x2C
        uint32_t        adler;      // 0x30
        uint32_t        reserved;   // 0x34
    };
    static_assert(sizeof(z_stream_game) == 56, "game z_stream must be 56 bytes");

    // zlib return codes
    static constexpr int ZR_OK         = 0;
    static constexpr int ZR_STREAM_END = 1;
    static constexpr int ZR_DATA_ERROR = -3;

    // ========================================================================
    // z_stream compatibility note (32-bit)
    //
    // On 32-bit Windows, sizeof(z_stream) = 56 bytes for both the game's
    // zlib 1.2.1 and chromium-zlib (uLong = unsigned long = 4 bytes).
    // We pass the game's z_stream directly — no proxy needed.
    // ========================================================================

    static_assert(sizeof(z_stream) == sizeof(z_stream_game),
        "z_stream size mismatch — proxy would be needed");

    // ========================================================================
    // Original function pointers (for baseline mode)
    // ========================================================================

    using inflateInit_fn  = int (__cdecl*)(void*, const char*, int);
    using inflate_fn      = int (__cdecl*)(void*, int);
    using inflateEnd_fn   = int (__cdecl*)(void*);

    static inflateInit_fn  s_origInflateInit  = nullptr;
    static inflate_fn      s_origInflate      = nullptr;
    static inflateEnd_fn   s_origInflateEnd   = nullptr;

    // ========================================================================
    // TESFile path — libdeflate single-shot
    //
    // ESP/ESM records: whole buffer, sizes known upfront, no streaming.
    // Eliminates inflateInit/inflateEnd overhead entirely.
    // ========================================================================

    static int __cdecl TESFile_InflateInit(void* strm, const char* /*version*/, int /*stream_size*/)
    {
        if (s_baselineMode)
            return s_origInflateInit(strm, "1.2.1", 56);

        auto* s = static_cast<z_stream_game*>(strm);
        // Store a non-null sentinel in state to distinguish from BSA path
        s->state = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEAD));
        s->total_in = 0;
        s->total_out = 0;
        s->msg = nullptr;
        return ZR_OK;
    }

    static int __cdecl TESFile_Inflate(void* strm, int flush)
    {
        if (s_baselineMode) {
            auto t0 = s_enableStats ? TrackCallTime() : 0;
            uint32_t outBefore = static_cast<z_stream_game*>(strm)->avail_out;
            int ret = s_origInflate(strm, flush);
            if (s_enableStats) {
                auto dt = QpcNow() - t0;
                s_formStats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
                s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                s_formStats.bytesOut.fetch_add(outBefore - static_cast<z_stream_game*>(strm)->avail_out,
                    std::memory_order_relaxed);
            }
            return ret;
        }

        auto* s = static_cast<z_stream_game*>(strm);

        if (!s->next_in || !s->next_out || s->avail_in == 0 || s->avail_out == 0) {
            return ZR_DATA_ERROR;
        }

        // Thread-local decompressor: allocated once per thread, reused across calls
        thread_local auto* tl_decompressor = libdeflate_alloc_decompressor();

        auto t0 = s_enableStats ? TrackCallTime() : 0;

        size_t actual_out = 0;
        enum libdeflate_result result = libdeflate_zlib_decompress(
            tl_decompressor,
            s->next_in,
            s->avail_in,
            s->next_out,
            s->avail_out,
            &actual_out
        );

        if (result != LIBDEFLATE_SUCCESS) {
            LogInfo("TESFile decompress failed: result=%d, in=%u, out_buf=%u",
                    static_cast<int>(result), s->avail_in, s->avail_out);
            return ZR_DATA_ERROR;
        }

        if (s_enableStats) {
            s_formStats.ticksTotal.fetch_add(QpcNow() - t0, std::memory_order_relaxed);
            s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
            s_formStats.bytesIn.fetch_add(s->avail_in, std::memory_order_relaxed);
            s_formStats.bytesOut.fetch_add(actual_out, std::memory_order_relaxed);
        }

        s->next_in  += s->avail_in;
        s->total_in += s->avail_in;
        s->avail_in  = 0;

        s->next_out  += actual_out;
        s->total_out += static_cast<uint32_t>(actual_out);
        s->avail_out -= static_cast<uint32_t>(actual_out);

        return ZR_STREAM_END;
    }

    static int __cdecl TESFile_InflateEnd(void* strm)
    {
        if (s_baselineMode)
            return s_origInflateEnd(strm);

        auto* s = static_cast<z_stream_game*>(strm);
        s->state = nullptr;
        return ZR_OK;
    }

    // ========================================================================
    // BSA path — libdeflate one-shot with chromium zlib streaming fallback
    //
    // On first inflate call, try libdeflate if all data fits. 93% of BSA
    // entries are handled this way. The remaining 7% use chromium zlib.
    // ========================================================================

    static int __cdecl BSA_InflateInit(void* strm, const char* /*version*/, int /*stream_size*/)
    {
        if (s_baselineMode)
            return s_origInflateInit(strm, "1.2.1", 56);

        auto* s = reinterpret_cast<z_stream*>(strm);
        return inflateInit2_(s, 15, zlibVersion(), static_cast<int>(sizeof(z_stream)));
    }

    static int __cdecl BSA_Inflate(void* strm, int flush)
    {
        if (s_baselineMode) {
            auto t0 = s_enableStats ? TrackCallTime() : 0;
            uint32_t outBefore = static_cast<z_stream_game*>(strm)->avail_out;
            int ret = s_origInflate(strm, flush);
            if (s_enableStats) {
                auto dt = QpcNow() - t0;
                s_streamStats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
                s_streamStats.calls.fetch_add(1, std::memory_order_relaxed);
                s_streamStats.bytesOut.fetch_add(outBefore - static_cast<z_stream_game*>(strm)->avail_out,
                    std::memory_order_relaxed);
            }
            return ret;
        }

        auto* s = reinterpret_cast<z_stream*>(strm);

        // On first call: try libdeflate one-shot if full buffers available.
        // We DON'T touch the zlib internal state — inflateEnd cleans it up normally.
        if (s->total_in == 0 && s->total_out == 0 && s->avail_out >= 256) {
            thread_local auto* tl_decompressor = libdeflate_alloc_decompressor();

            size_t actual_out = 0;
            auto result = libdeflate_zlib_decompress(
                tl_decompressor,
                s->next_in, s->avail_in,
                s->next_out, s->avail_out,
                &actual_out);

            if (result == LIBDEFLATE_SUCCESS && actual_out <= static_cast<size_t>(s->avail_out)) {
                if (s_enableStats) {
                    auto t0 = TrackCallTime();
                    s_formStats.ticksTotal.fetch_add(QpcNow() - t0, std::memory_order_relaxed);
                    s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                    s_formStats.bytesIn.fetch_add(s->avail_in, std::memory_order_relaxed);
                    s_formStats.bytesOut.fetch_add(actual_out, std::memory_order_relaxed);
                    s_libdeflateBsaHits.fetch_add(1, std::memory_order_relaxed);
                }

                s->total_in  = s->avail_in;
                s->total_out = static_cast<uLong>(actual_out);
                s->next_in  += s->avail_in;
                s->avail_in  = 0;
                s->next_out += actual_out;
                s->avail_out -= static_cast<uInt>(actual_out);

                return ZR_STREAM_END;
            }
            if (s_enableStats)
                s_libdeflateBsaMisses.fetch_add(1, std::memory_order_relaxed);
        }

        // Chromium zlib streaming fallback (SSE2 SIMD chunk copy)
        if (!s_enableStats)
            return inflate(s, flush);

        auto t0 = TrackCallTime();
        uint32_t inBefore  = s->avail_in;
        uint32_t outBefore = s->avail_out;

        int ret = inflate(s, flush);

        s_streamStats.ticksTotal.fetch_add(QpcNow() - t0, std::memory_order_relaxed);
        s_streamStats.calls.fetch_add(1, std::memory_order_relaxed);
        s_streamStats.bytesIn.fetch_add(inBefore - s->avail_in, std::memory_order_relaxed);
        s_streamStats.bytesOut.fetch_add(outBefore - s->avail_out, std::memory_order_relaxed);

        return ret;
    }

    static int __cdecl BSA_InflateEnd(void* strm)
    {
        if (s_baselineMode)
            return s_origInflateEnd(strm);

        return inflateEnd(reinterpret_cast<z_stream*>(strm));
    }

    // ========================================================================
    // Memory patching utilities for 32-bit FNV
    // ========================================================================

    static void SafeWrite32(uint32_t addr, uint32_t value) {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &oldProtect);
        *reinterpret_cast<uint32_t*>(addr) = value;
        VirtualProtect(reinterpret_cast<void*>(addr), 4, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 4);
    }

    // Patch a CALL instruction: preserve the E8 opcode, rewrite the relative offset
    template <typename T>
    static void ReplaceCall(uint32_t callAddr, T newTarget) {
        uint32_t target = reinterpret_cast<uint32_t>(newTarget);
        SafeWrite32(callAddr + 1, target - callAddr - 5);
    }

    // Read the current CALL target from a CALL instruction
    static uint32_t ReadCallTarget(uint32_t callAddr) {
        int32_t rel = *reinterpret_cast<int32_t*>(callAddr + 1);
        return callAddr + 5 + rel;
    }

    // ========================================================================
    // Hook installation
    // ========================================================================

    void Install(bool isEditor)
    {
        LoadINI();
        LogOpen();

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_qpcFreq = freq.QuadPart;
        s_installTime = QpcNow();

        LogInfo("FastDecompressNV v%s initializing (%s)", kVersion, isEditor ? "GECK" : "runtime");
        LogInfo("  libdeflate: single-shot form decompression");
        LogInfo("  chromium-zlib %s: SIMD streaming BSA decompression", zlibVersion());
        LogInfo("  stats=%s  mode=%s",
            s_enableStats ? "ON" : "OFF",
            s_baselineMode ? "BASELINE" : "OPTIMIZED");



        if (!isEditor) {
            // Save original function pointers for baseline mode
            s_origInflateInit = reinterpret_cast<inflateInit_fn>(ReadCallTarget(0x4742AC));
            s_origInflate     = reinterpret_cast<inflate_fn>(ReadCallTarget(0x47434F));
            s_origInflateEnd  = reinterpret_cast<inflateEnd_fn>(ReadCallTarget(0x4742CA));

            LogInfo("Original zlib: inflateInit_=%08Xh  inflate=%08Xh  inflateEnd=%08Xh",
                reinterpret_cast<uint32_t>(s_origInflateInit),
                reinterpret_cast<uint32_t>(s_origInflate),
                reinterpret_cast<uint32_t>(s_origInflateEnd));

            // --- TESFile::DecompressCurrentForm (libdeflate single-shot) ---
            ReplaceCall(0x4742AC, TESFile_InflateInit);
            ReplaceCall(0x47434F, TESFile_Inflate);
            for (uint32_t addr : { 0x4742CAu, 0x474388u, 0x4743D5u, 0x474419u })
                ReplaceCall(addr, TESFile_InflateEnd);

            // --- CompressedArchiveFile BSA (libdeflate one-shot + chromium-zlib streaming) ---
            ReplaceCall(0xAFC537, BSA_InflateInit);
            ReplaceCall(0xAFC1F4, BSA_Inflate);
            for (uint32_t addr : { 0xAFC00Eu, 0xAFC21Bu, 0xAFC552u })
                ReplaceCall(addr, BSA_InflateEnd);

            LogInfo("Runtime hooks installed (6 call sites patched)");
            LogInfo("  TESFile: libdeflate single-shot");
            LogInfo("  BSA:     libdeflate one-shot + chromium-zlib %s SIMD fallback", zlibVersion());
        } else {
            // GECK editor
            s_origInflateInit = reinterpret_cast<inflateInit_fn>(ReadCallTarget(0x4DFB34));
            s_origInflate     = reinterpret_cast<inflate_fn>(ReadCallTarget(0x4DFB9E));
            s_origInflateEnd  = reinterpret_cast<inflateEnd_fn>(ReadCallTarget(0x4DFB45));

            LogInfo("Original zlib: inflateInit_=%08Xh  inflate=%08Xh  inflateEnd=%08Xh",
                reinterpret_cast<uint32_t>(s_origInflateInit),
                reinterpret_cast<uint32_t>(s_origInflate),
                reinterpret_cast<uint32_t>(s_origInflateEnd));

            // --- TESFile::DecompressCurrentForm (libdeflate single-shot) ---
            ReplaceCall(0x4DFB34, TESFile_InflateInit);
            ReplaceCall(0x4DFB9E, TESFile_Inflate);
            for (uint32_t addr : { 0x4DFB45u, 0x4DFC1Fu, 0x4DFBD5u, 0x4DFBC4u })
                ReplaceCall(addr, TESFile_InflateEnd);

            // --- CompressedArchiveFile BSA (libdeflate one-shot + chromium-zlib streaming) ---
            ReplaceCall(0x8AAF17, BSA_InflateInit);
            ReplaceCall(0x8AABD4, BSA_Inflate);
            for (uint32_t addr : { 0x8AA9EEu, 0x8AABFBu, 0x8AAF32u })
                ReplaceCall(addr, BSA_InflateEnd);

            LogInfo("GECK editor hooks installed (6 call sites patched)");
        }

        if (!s_enableStats) return;

        // Background thread: log stats at increasing intervals
        std::thread([]() {
            int intervals[] = {5, 5, 5, 5, 5, 5, 15, 15};
            for (int s : intervals) {
                std::this_thread::sleep_for(std::chrono::seconds(s));
                if (s_stopLogging.load(std::memory_order_relaxed)) return;
                LogStats();
            }
            int idleCount = 0;
            while (!s_stopLogging.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                if (s_stopLogging.load(std::memory_order_relaxed)) return;
                LogStats();

                auto last = s_lastCallQpc.load(std::memory_order_relaxed);
                if (last > 0 && TicksToMs(QpcNow() - last) > 60000.0) {
                    idleCount++;
                    if (idleCount >= 5) {
                        LogInfo("FastDecompressNV: stopping stats - idle for 5+ minutes");
                        s_stopLogging.store(true, std::memory_order_relaxed);
                        return;
                    }
                } else {
                    idleCount = 0;
                }
            }
        }).detach();
    }
}
