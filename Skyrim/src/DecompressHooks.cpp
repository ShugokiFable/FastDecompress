#include "PCH.h"
#include "DecompressHooks.h"

#include <libdeflate.h>
#include <zlib.h>  // zlib-ng compat mode — standard zlib API, SIMD-optimized
#include <lz4.h>   // LZ4 v1.10.0
#include <immintrin.h> // AVX2 + prefetch intrinsics
#include <detours/detours.h>

namespace FastDecompress
{
    // ========================================================================
    // Build mode: set to true for baseline (passthrough) measurements,
    //             false for optimized (zlib-ng + libdeflate + LZ4 v1.10).
    // ========================================================================

    static constexpr bool BASELINE_MODE = false;
    static bool s_enableStats = false; // loaded from INI at runtime

    // ========================================================================
    // QPC timing helpers
    // ========================================================================

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

    // Prefetch source data into L1 cache to avoid cold-start stalls.
    // Hardware prefetcher handles the rest once sequential pattern is established.
    static void PrefetchSource(const void* addr, std::size_t bytes) {
        const char* p = static_cast<const char*>(addr);
        const char* end = p + (std::min)(bytes, std::size_t(32768)); // first 32KB
        for (; p < end; p += 64)
            _mm_prefetch(p, _MM_HINT_T0);
    }

    // ========================================================================
    // Per-path stats (lock-free atomics)
    // ========================================================================

    struct Stats {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> bytesOut{0};
        std::atomic<int64_t>  ticks{0};
    };

    static Stats s_formStats;    // libdeflate one-shot (form decompression)
    static Stats s_streamStats;  // zlib-ng streaming inflate
    static Stats s_lz4Stats;     // LZ4 v1.10.0 (BSA assets)
    static std::atomic<uint64_t> s_lz4DictCalls{0};   // LZ4 generic calls with dictSize > 0
    static std::atomic<uint64_t> s_lz4NoDictCalls{0};  // LZ4 generic calls with dictSize == 0
    static std::atomic<uint64_t> s_stockZlibCalls{0};  // pass-through to original zlib (should be 0)

    // Wall clock tracking: first and last decompression call timestamps
    static std::atomic<int64_t> s_firstCallQpc{0};
    static std::atomic<int64_t> s_lastCallQpc{0};

    static int64_t TrackCallTime() {
        auto now = QpcNow();
        int64_t expected = 0;
        s_firstCallQpc.compare_exchange_strong(expected, now, std::memory_order_relaxed);
        s_lastCallQpc.store(now, std::memory_order_relaxed);
        return now;
    }

    // Snapshot for delta computation between log intervals
    struct Snapshot {
        uint64_t formCalls, formOut;
        int64_t  formTicks;
        uint64_t streamCalls, streamOut;
        int64_t  streamTicks;
        uint64_t lz4Calls, lz4Out;
        int64_t  lz4Ticks;
        double   elapsed;
    };
    static Snapshot s_prev{};          // only accessed from the logging thread
    static DWORD s_logThreadId = 0;   // set once by the logging thread

    void LogStats()
    {
        // Guard: s_prev is not thread-safe — assert single-thread access
        DWORD tid = GetCurrentThreadId();
        DWORD expected = 0;
        if (!s_logThreadId) s_logThreadId = tid;
        assert(s_logThreadId == tid && "LogStats must only be called from the logging thread");
        double elapsed = TicksToMs(QpcNow() - s_installTime);

        auto fc = s_formStats.calls.load();
        auto fbo = s_formStats.bytesOut.load();
        auto ftRaw = s_formStats.ticks.load();
        auto ft = TicksToMs(ftRaw);

        auto sc = s_streamStats.calls.load();
        auto sbo = s_streamStats.bytesOut.load();
        auto stRaw = s_streamStats.ticks.load();
        auto st = TicksToMs(stRaw);

        auto lc = s_lz4Stats.calls.load();
        auto lbo = s_lz4Stats.bytesOut.load();
        auto ltRaw = s_lz4Stats.ticks.load();
        auto lt = TicksToMs(ltRaw);

        double totalMs = ft + st + lt;
        double totalMB = (fbo + sbo + lbo) / (1024.0 * 1024.0);

        // --- Cumulative ---
        logger::info("=== FastDecompress Stats ({:.1f}s, mode={}) ===",
            elapsed / 1000.0, BASELINE_MODE ? "BASELINE" : "OPTIMIZED");
        logger::info("  form (libdeflate):  {:6} calls  {:8.1f} MB out  {:8.1f} ms CPU",
            fc, fbo / (1024.0 * 1024.0), ft);
        logger::info("  stream (zlib-ng):   {:6} calls  {:8.1f} MB out  {:8.1f} ms CPU",
            sc, sbo / (1024.0 * 1024.0), st);
        logger::info("  LZ4 (BSA):          {:6} calls  {:8.1f} MB out  {:8.1f} ms CPU",
            lc, lbo / (1024.0 * 1024.0), lt);

        auto dictCalls = s_lz4DictCalls.load();
        auto noDictCalls = s_lz4NoDictCalls.load();
        if (dictCalls + noDictCalls > 0) {
            logger::info("  LZ4 blocks: {} independent, {} linked (dict)",
                noDictCalls, dictCalls);
        }
        auto stockCalls = s_stockZlibCalls.load();
        if (stockCalls > 0) {
            logger::warn("  stock zlib pass-through: {} calls (pre-hook streams)", stockCalls);
        }

        if (totalMs > 0) {
            logger::info("  TOTAL: {:.1f} ms CPU  {:.1f} MB  {:.0f} MB/s",
                totalMs, totalMB, totalMB / (totalMs / 1000.0));
        }

        // --- Wall clock loading phase ---
        auto first = s_firstCallQpc.load();
        auto last = s_lastCallQpc.load();
        if (first > 0 && last > first) {
            double wallMs = TicksToMs(last - first);
            logger::info("  wall clock: first→last call = {:.1f} ms  (CPU/wall = {:.1f}%)",
                wallMs, totalMs > 0 ? (totalMs / wallMs * 100.0) : 0.0);
        }

        // --- Delta since last log (streaming activity) ---
        if (s_prev.elapsed > 0) {
            double dt = elapsed - s_prev.elapsed;
            uint64_t dCalls = (fc - s_prev.formCalls) + (sc - s_prev.streamCalls) + (lc - s_prev.lz4Calls);
            double dMB = ((fbo - s_prev.formOut) + (sbo - s_prev.streamOut) + (lbo - s_prev.lz4Out))
                         / (1024.0 * 1024.0);
            double dCpuMs = TicksToMs((ftRaw - s_prev.formTicks) + (stRaw - s_prev.streamTicks) +
                                      (ltRaw - s_prev.lz4Ticks));
            if (dCalls > 0) {
                logger::info("  delta ({:.0f}s): {} calls  {:.1f} MB  {:.1f} ms CPU  ({:.1f} ms CPU/min)",
                    dt / 1000.0, dCalls, dMB, dCpuMs, dt > 0 ? (dCpuMs / (dt / 60000.0)) : 0.0);
            } else {
                logger::info("  delta ({:.0f}s): idle (no decompression activity)",
                    dt / 1000.0);
            }
        }

        // Save snapshot
        s_prev = { fc, fbo, ftRaw, sc, sbo, stRaw, lc, lbo, ltRaw, elapsed };
    }

    // ========================================================================
    // zlib return codes
    // ========================================================================

    static constexpr int ZR_OK = 0;
    static constexpr int ZR_STREAM_END = 1;

    // ========================================================================
    // Magic tag for zlib-ng streams
    // ========================================================================

    static constexpr uLong kZlibNgMagic = 0xFD5E'A110;

    // ========================================================================
    // Trampolines — original zlib 1.2.7 functions
    // ========================================================================

    using fn_inflate      = int (__cdecl*)(z_streamp, int);
    using fn_inflateEnd   = int (__cdecl*)(z_streamp);
    using fn_inflateReset = int (__cdecl*)(z_streamp);           // Skyrim: 1 param only
    // SE/AE: inflateInit_ (strm, version, stream_size) — 3 params
    using fn_inflateInit_SE = int (__cdecl*)(z_streamp, const char*, int);
    // VR: inflateInit2_ (strm, windowBits, version, stream_size) — 4 params
    using fn_inflateInit_VR = int (__cdecl*)(z_streamp, int, const char*, int);
    // Union type for Detours (must be one pointer). 4-param covers both; on x64
    // Windows ABI, passing extra register args to a 3-param function is harmless.
    using fn_inflateInit  = int (__cdecl*)(z_streamp, std::uintptr_t, std::uintptr_t, int);
    using fn_LZ4_decompress_safe = int (__cdecl*)(const char*, char*, int, int);
    // LZ4_decompress_generic: 6-param internal function used by BSResource::CompressedArchiveStream
    // (src, dst, srcSize, outputSize, dictStart, dictSize)
    using fn_LZ4_decompress_generic = int (__cdecl*)(const char*, char*, int, int, const char*, int);

    static fn_inflate      s_origInflate      = nullptr;
    static fn_inflateEnd   s_origInflateEnd   = nullptr;
    static fn_inflateReset s_origInflateReset = nullptr;
    static fn_inflateInit  s_origInflateInit  = nullptr;
    static fn_LZ4_decompress_safe s_origLZ4   = nullptr;
    static fn_LZ4_decompress_generic s_origLZ4Generic = nullptr;
    static bool s_isVR = false;  // set during Install(), used for correct baseline forwarding

    // (Trampolines managed by Microsoft Detours)

    // ========================================================================
    // Global zlib hooks (zlib-ng vs original passthrough)
    //
    // Skyrim's zlib uses inflateInit_ (3 params, hardcoded wbits=15)
    // and inflateReset (1 param). NOT inflateInit2_ (4 params).
    // ========================================================================

    // Global inflateInit hook — 4 params to handle both SE and VR.
    // SE/AE callers pass (strm, version, stream_size); VR passes (strm, wbits, version, stream_size).
    // In baseline mode we forward with the correct arity per runtime.
    static int __cdecl Hook_inflateInit(z_streamp strm,
        std::uintptr_t p2, std::uintptr_t p3, int p4)
    {
        if constexpr (BASELINE_MODE) {
            if (s_isVR) {
                return reinterpret_cast<fn_inflateInit_VR>(s_origInflateInit)(
                    strm, static_cast<int>(p2), reinterpret_cast<const char*>(p3), p4);
            } else {
                return reinterpret_cast<fn_inflateInit_SE>(s_origInflateInit)(
                    strm, reinterpret_cast<const char*>(p2), static_cast<int>(p3));
            }
        } else {
            int ret = inflateInit2_(strm, 15, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
            if (ret == ZR_OK) strm->reserved = kZlibNgMagic;
            return ret;
        }
    }

    static int __cdecl Hook_inflate(z_streamp strm, int flush)
    {
        // #2: Only prefetch on first call; continuations already have data warm in cache
        if constexpr (!BASELINE_MODE) {
            if (strm->total_in == 0)
                PrefetchSource(strm->next_in, strm->avail_in);
        }
        [[maybe_unused]] auto t0 = s_enableStats ? TrackCallTime() : 0;
        [[maybe_unused]] uint32_t outBefore = s_enableStats ? strm->avail_out : 0;

        int ret;
        if constexpr (BASELINE_MODE) {
            ret = s_origInflate(strm, flush);
        } else {
            // Auto-detect one-shot (form) decompression: try libdeflate on first call.
            // #4: Skip if avail_out < 256 — too small for a complete form, must be streaming.
            if (strm->total_in == 0 && strm->total_out == 0 &&
                strm->reserved == kZlibNgMagic && strm->avail_out >= 256) {
                std::size_t outBytes = 0;
                struct DecompDeleter { void operator()(libdeflate_decompressor* d) { libdeflate_free_decompressor(d); } };
                thread_local std::unique_ptr<libdeflate_decompressor, DecompDeleter> tl_decompressor(libdeflate_alloc_decompressor());
                auto result = libdeflate_zlib_decompress(
                    tl_decompressor.get(),
                    strm->next_in, strm->avail_in,
                    strm->next_out, strm->avail_out,
                    &outBytes);

                if (result == LIBDEFLATE_SUCCESS && outBytes <= strm->avail_out) {
                    auto consumed = strm->avail_in;
                    strm->next_in += consumed;
                    strm->avail_in = 0;
                    strm->next_out += outBytes;
                    strm->avail_out -= static_cast<uInt>(outBytes);
                    strm->total_in = consumed;
                    strm->total_out = static_cast<uLong>(outBytes);

                    if (s_enableStats) {
                        auto dt = QpcNow() - t0;
                        s_formStats.ticks.fetch_add(dt, std::memory_order_relaxed);
                        s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                        s_formStats.bytesOut.fetch_add(outBytes, std::memory_order_relaxed);
                    }
                    return ZR_STREAM_END;
                }
            }

            // Streaming path: zlib-ng for our streams, original zlib for pre-hook streams
            if (strm->reserved == kZlibNgMagic) {
                ret = inflate(strm, flush);
            } else {
                if (s_enableStats) s_stockZlibCalls.fetch_add(1, std::memory_order_relaxed);
                ret = s_origInflate(strm, flush);
            }
        }

        if (s_enableStats) {
            auto dt = QpcNow() - t0;
            s_streamStats.ticks.fetch_add(dt, std::memory_order_relaxed);
            s_streamStats.calls.fetch_add(1, std::memory_order_relaxed);
            s_streamStats.bytesOut.fetch_add(outBefore - strm->avail_out, std::memory_order_relaxed);
        }
        return ret;
    }

    static int __cdecl Hook_inflateEnd(z_streamp strm)
    {
        if (strm->reserved == kZlibNgMagic) {
            strm->reserved = 0;
            return inflateEnd(strm);
        }
        return s_origInflateEnd(strm);
    }

    // inflateReset hook: (strm) — 1 param only in Skyrim
    static int __cdecl Hook_inflateReset(z_streamp strm)
    {
        if (strm->reserved == kZlibNgMagic)
            return inflateReset(strm);
        return s_origInflateReset(strm);
    }

    // ========================================================================
    // LZ4 hooks (v1.10 vs original passthrough)
    // ========================================================================

    static std::atomic<bool> s_stopLogging{false};
    static bool s_lz4Hooked = false;
    static bool s_lz4GenericHooked = false;

    static int __cdecl Hook_LZ4_decompress_safe(const char* src, char* dst,
        int compressedSize, int dstCapacity)
    {
        if constexpr (!BASELINE_MODE) PrefetchSource(src, compressedSize);
        [[maybe_unused]] auto t0 = s_enableStats ? TrackCallTime() : 0;

        int ret;
        if constexpr (BASELINE_MODE) {
            ret = s_origLZ4(src, dst, compressedSize, dstCapacity);
        } else {
            ret = LZ4_decompress_safe(src, dst, compressedSize, dstCapacity);
        }

        if (s_enableStats) {
            auto dt = QpcNow() - t0;
            s_lz4Stats.ticks.fetch_add(dt, std::memory_order_relaxed);
            s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
            if (ret > 0)
                s_lz4Stats.bytesOut.fetch_add(ret, std::memory_order_relaxed);
        }
        return ret;
    }

    // LZ4_decompress_generic: the 6-param internal function called by
    // BSResource::CompressedArchiveStream for LZ4 frame block decompression.
    // This is the main BSA decompression path — handles the bulk of game data.
    static int __cdecl Hook_LZ4_decompress_generic(const char* src, char* dst,
        int srcSize, int outputSize, const char* dictStart, int dictSize)
    {
        if constexpr (!BASELINE_MODE) {
            PrefetchSource(src, srcSize);
            // #3: Prefetch dictionary data too — it will be read during decompression
            if (dictSize > 0 && dictStart != nullptr)
                PrefetchSource(dictStart, dictSize);
        }
        [[maybe_unused]] auto t0 = s_enableStats ? TrackCallTime() : 0;

        int ret;
        if constexpr (BASELINE_MODE) {
            ret = s_origLZ4Generic(src, dst, srcSize, outputSize, dictStart, dictSize);
        } else {
            if (dictSize > 0 && dictStart != nullptr) {
                ret = LZ4_decompress_safe_usingDict(src, dst, srcSize, outputSize, dictStart, dictSize);
            } else {
                ret = LZ4_decompress_safe(src, dst, srcSize, outputSize);
            }
        }

        if (s_enableStats) {
            auto dt = QpcNow() - t0;
            s_lz4Stats.ticks.fetch_add(dt, std::memory_order_relaxed);
            s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
            if (ret > 0)
                s_lz4Stats.bytesOut.fetch_add(ret, std::memory_order_relaxed);
            if (dictSize > 0)
                s_lz4DictCalls.fetch_add(1, std::memory_order_relaxed);
            else
                s_lz4NoDictCalls.fetch_add(1, std::memory_order_relaxed);
        }
        return ret;
    }

    // ========================================================================
    // Signature scanning (VR only)
    // ========================================================================

    static std::uintptr_t ScanForBytes(std::uintptr_t start, std::size_t length,
        const std::uint8_t* pattern, std::size_t patLen,
        std::size_t* nextStart = nullptr)
    {
        const auto* data = reinterpret_cast<const std::uint8_t*>(start);
        std::size_t begin = nextStart ? *nextStart : 0;
        for (std::size_t i = begin; i + patLen <= length; i++) {
            if (std::memcmp(data + i, pattern, patLen) == 0) {
                if (nextStart) *nextStart = i + 1;
                return start + i;
            }
        }
        return 0;
    }

    static bool VerifyBytes(std::uintptr_t addr, const std::uint8_t* expected, std::size_t len)
    {
        return std::memcmp(reinterpret_cast<const void*>(addr), expected, len) == 0;
    }

    // Check if an address points to a dead stub (ret / ret N / int3 padding).
    // SE 1.5.97 Address Library maps some IDs to empty stubs instead of real functions.
    static bool IsStubFunction(std::uintptr_t addr)
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(addr);
        // C3 = ret, C2 xx xx = ret N, CC = int3 (padding/dead code)
        return p[0] == 0xC3 || p[0] == 0xC2 || p[0] == 0xCC;
    }

    // ========================================================================
    // Signature tables (VR + SE 1.5.97 fallback)
    // ========================================================================

    // --- inflate prologue (common across versions): mov [rsp+10h],edx; mov [rsp+8],rcx ---
    static constexpr std::uint8_t kSig_inflatePrefix[] = {
        0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08,
    };

    // --- inflateEnd prologue ---
    static constexpr std::uint8_t kSig_inflateEnd_v1[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    };

    // --- inflateInit2_ signatures per version ---
    // VR: mov [rsp+18h],rbx; push rsi; sub rsp,20h
    static constexpr std::uint8_t kSig_inflateInit2_v1[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20,
    };

    // --- inflateReset signatures ---
    // VR: test rcx,rcx; jz xx; mov rax,[rcx+28h]
    static constexpr std::uint8_t kSig_inflateReset_VR[] = {
        0x48, 0x85, 0xC9,
    };

    // --- Cluster 2 signatures (VR second inflate copy) ---
    static constexpr std::uint8_t kSig_inflate_v2[] = {
        0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20,
        0x44, 0x8B, 0xF2, 0x48, 0x8B, 0xF9, 0x48, 0x85, 0xC9,
    };
    static constexpr std::uint8_t kSig_inflateEnd_v2[] = {
        0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    };
    static constexpr std::uint8_t kSig_inflateInit2_v3[] = {
        0x53, 0x55, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x20,
    };
    static constexpr std::uint8_t kSig_inflateReset_v2[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9,
    };

    // --- Offset tables ---
    struct InflateOffsets {
        std::ptrdiff_t toEnd;
        std::ptrdiff_t toInit2;
        std::ptrdiff_t toReset;
        const std::uint8_t* init2Sig;
        std::size_t init2SigLen;
        const std::uint8_t* resetSig;
        std::size_t resetSigLen;
    };

    static constexpr InflateOffsets kOffsets_VR = {
        0x19E0, 0x1A70, 0x1C30,
        kSig_inflateInit2_v1, sizeof(kSig_inflateInit2_v1),
        kSig_inflateReset_VR, sizeof(kSig_inflateReset_VR)
    };
    static constexpr InflateOffsets kAllOffsets_c1[] = { kOffsets_VR };

    static constexpr InflateOffsets kOffsets_c2_VR = {
        0xCA0, 0xD80, 0x1280,
        kSig_inflateInit2_v3, sizeof(kSig_inflateInit2_v3),
        kSig_inflateReset_v2, sizeof(kSig_inflateReset_v2)
    };
    static constexpr InflateOffsets kAllOffsets_c2[] = { kOffsets_c2_VR };

    // --- Cluster storage ---
    struct InflateCluster {
        std::uintptr_t inflate;
        std::uintptr_t inflateEnd;
        std::uintptr_t inflateInit2;
        std::uintptr_t inflateReset;
    };
    static constexpr int kMaxClusters = 4;
    static InflateCluster s_clusters[kMaxClusters];
    static int s_clusterCount = 0;

    // LZ4_decompress_safe: mov eax, 4068h; call __alloca_probe
    static constexpr std::uint8_t kSig_LZ4_decompress_safe[] = {
        0xB8, 0x68, 0x40, 0x00, 0x00, 0xE8,
    };

    // LZ4_decompress_generic: 6-param internal function (BSA frame block decompressor)
    static constexpr std::uint8_t kSig_LZ4_decompress_generic[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x8D, 0x6C, 0x24, 0xE9, 0x48, 0x81, 0xEC,
        0xE0, 0x00, 0x00, 0x00,
    };

    // ========================================================================
    // Install
    // ========================================================================

    static bool FindCluster(
        std::uintptr_t textBase, std::size_t textSize,
        const std::uint8_t* inflateSig, std::size_t inflateSigLen,
        const InflateOffsets* offsetTables, std::size_t numTables,
        const std::uint8_t* endSig, std::size_t endSigLen)
    {
        auto textEnd = textBase + textSize;
        std::size_t scanPos = 0;
        while (true) {
            auto inflateAddr = ScanForBytes(textBase, textSize,
                inflateSig, inflateSigLen, &scanPos);
            if (!inflateAddr) break;

            for (std::size_t t = 0; t < numTables; t++) {
                const auto& offsets = offsetTables[t];
                auto endAddr   = inflateAddr + offsets.toEnd;
                auto init2Addr = inflateAddr + offsets.toInit2;
                auto resetAddr = inflateAddr + offsets.toReset;
                if (resetAddr + 16 > textEnd) continue;

                if (VerifyBytes(endAddr, endSig, endSigLen) &&
                    VerifyBytes(init2Addr, offsets.init2Sig, offsets.init2SigLen) &&
                    VerifyBytes(resetAddr, offsets.resetSig, offsets.resetSigLen))
                {
                    if (s_clusterCount < kMaxClusters) {
                        s_clusters[s_clusterCount++] = {
                            inflateAddr, endAddr, init2Addr, resetAddr
                        };
                        logger::info("inflate cluster {} at {:x} (end={:x} init2={:x} reset={:x})",
                            s_clusterCount, inflateAddr, endAddr, init2Addr, resetAddr);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // ========================================================================
    // Address Library IDs for SE/AE (stable across all versions)
    // ========================================================================

    static constexpr REL::ID kID_inflate(56875);
    static constexpr REL::ID kID_inflateEnd(56877);
    static constexpr REL::ID kID_inflateInit2(56881);
    static constexpr REL::ID kID_inflateReset(56888);
    static constexpr REL::ID kID_LZ4_decompress_generic(110040);

    // Follow jump thunks to find the real function body.
    // SE 1.5.97 Address Library points to small jump stubs (thunks), not the
    // actual function code. Hooking a thunk that the game never calls through
    // results in 0 intercepted calls. This resolves the chain to the real target.
    static std::uintptr_t ResolveThunk(std::uintptr_t addr)
    {
        // Follow chains of thunks (thunk → thunk → real function).
        // Cap iterations to avoid infinite loops on malformed code.
        for (int i = 0; i < 8; i++) {
            const auto* p = reinterpret_cast<const std::uint8_t*>(addr);

            // E9 xx xx xx xx = jmp rel32
            if (p[0] == 0xE9) {
                auto offset = *reinterpret_cast<const std::int32_t*>(p + 1);
                addr = addr + 5 + offset;
                continue;
            }

            // FF 25 xx xx xx xx = jmp [rip+disp32]  (indirect jump)
            if (p[0] == 0xFF && p[1] == 0x25) {
                auto disp = *reinterpret_cast<const std::int32_t*>(p + 2);
                addr = *reinterpret_cast<const std::uintptr_t*>(addr + 6 + disp);
                continue;
            }

            break; // not a thunk — already the real function
        }
        return addr;
    }

    // Detours helper: attach a single hook, logging success/failure.
    static bool DetourAttachHook(void** ppOriginal, void* pHook, const char* name)
    {
        LONG err = DetourAttach(ppOriginal, pHook);
        if (err != NO_ERROR) {
            logger::error("DetourAttach({}) failed: error {}", name, err);
            return false;
        }
        logger::info("  hooked {} at {:x}", name, reinterpret_cast<std::uintptr_t>(*ppOriginal));
        return true;
    }

    // Load settings from FastDecompressSkyrim.ini (next to the DLL)
    static void LoadINI()
    {
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&LoadINI), &hm);
        char dllPath[MAX_PATH]{};
        GetModuleFileNameA(hm, dllPath, MAX_PATH);
        std::filesystem::path iniPath = std::filesystem::path(dllPath).replace_extension(".ini");

        s_enableStats = GetPrivateProfileIntA("General", "bEnableLogging", 0, iniPath.string().c_str()) != 0;
    }

    void Install()
    {
        LoadINI();

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_qpcFreq = freq.QuadPart;
        s_installTime = QpcNow();

        const bool isVR = REL::Module::IsVR();
        s_isVR = isVR;
        const char* runtime = isVR ? "VR" : "SE/AE";

        logger::info("FastDecompressSkyrim v{} mode={} runtime={} game={} base={:x}",
            kVersion,
            BASELINE_MODE ? "BASELINE" : "OPTIMIZED",
            runtime, REL::Module::get().version().string(),
            REL::Module::get().base());

        auto textSeg = REL::Module::get().segment(REL::Segment::textx);
        const auto textBase = textSeg.address();
        const auto textSize = textSeg.size();

        // ============================================================
        // Step 1: Locate function addresses
        // SE/AE: Address Library
        // VR: Signature scanning
        // ============================================================

        bool useAddressLib = false;
        if (!isVR) {
            // Try Address Library first. On AE it returns real function bodies.
            // On SE 1.5.97 it returns dead stubs (C2 00 00 = ret 0).
            auto inflateAddr = ResolveThunk(kID_inflate.address());
            if (!IsStubFunction(inflateAddr)) {
                s_clusters[0] = {
                    inflateAddr,
                    ResolveThunk(kID_inflateEnd.address()),
                    ResolveThunk(kID_inflateInit2.address()),
                    ResolveThunk(kID_inflateReset.address())
                };
                s_clusterCount = 1;
                useAddressLib = true;
                auto& c = s_clusters[0];
                logger::info("inflate cluster 1 via AddressLib at {:x} (end={:x} init2={:x} reset={:x})",
                    c.inflate, c.inflateEnd, c.inflateInit2, c.inflateReset);
            } else {
                logger::warn("AddressLib inflate points to stub ({:x}) — falling back to signature scan",
                    inflateAddr);
            }
        }

        if (!useAddressLib) {
            // Signature scanning: VR always, SE 1.5.97 as fallback
            FindCluster(textBase, textSize,
                kSig_inflatePrefix, sizeof(kSig_inflatePrefix),
                kAllOffsets_c1, std::size(kAllOffsets_c1),
                kSig_inflateEnd_v1, sizeof(kSig_inflateEnd_v1));

            FindCluster(textBase, textSize,
                kSig_inflate_v2, sizeof(kSig_inflate_v2),
                kAllOffsets_c2, std::size(kAllOffsets_c2),
                kSig_inflateEnd_v2, sizeof(kSig_inflateEnd_v2));
        }

        if (s_clusterCount == 0)
            logger::warn("could not find any inflate cluster — zlib hooks NOT installed");
        else
            logger::info("found {} inflate cluster(s)", s_clusterCount);

        // ============================================================
        // Step 2: Find LZ4 addresses
        // ============================================================

        std::uintptr_t lz4SafeAddr = 0;
        {
            std::size_t scanPos = 0;
            lz4SafeAddr = ScanForBytes(textBase, textSize,
                kSig_LZ4_decompress_safe, sizeof(kSig_LZ4_decompress_safe), &scanPos);
        }

        std::uintptr_t lz4GenAddr = 0;
        if (useAddressLib) {
            auto raw = kID_LZ4_decompress_generic.address();
            auto resolved = ResolveThunk(raw);
            // Verify resolved address is within the game module (not a system DLL)
            if (resolved >= textBase && resolved < textBase + textSize) {
                lz4GenAddr = resolved;
                logger::info("LZ4_decompress_generic via AddressLib at {:x}", lz4GenAddr);
            } else {
                logger::warn("AddressLib LZ4_decompress_generic resolved outside game module ({:x}) — using sig scan",
                    resolved);
            }
        }
        if (!lz4GenAddr) {
            std::size_t scanPos = 0;
            lz4GenAddr = ScanForBytes(textBase, textSize,
                kSig_LZ4_decompress_generic, sizeof(kSig_LZ4_decompress_generic), &scanPos);
            if (lz4GenAddr)
                logger::info("LZ4_decompress_generic via sig scan at {:x}", lz4GenAddr);
        }

        // ============================================================
        // Step 3: Install all hooks via Microsoft Detours
        // ============================================================

        // Set original pointers to the resolved addresses (Detours requires this)
        if (s_clusterCount > 0) {
            auto& c = s_clusters[0];
            s_origInflate      = reinterpret_cast<fn_inflate>(c.inflate);
            s_origInflateEnd   = reinterpret_cast<fn_inflateEnd>(c.inflateEnd);
            s_origInflateInit  = reinterpret_cast<fn_inflateInit>(c.inflateInit2);
            s_origInflateReset = reinterpret_cast<fn_inflateReset>(c.inflateReset);
        }
        if (lz4SafeAddr)
            s_origLZ4 = reinterpret_cast<fn_LZ4_decompress_safe>(lz4SafeAddr);
        if (lz4GenAddr)
            s_origLZ4Generic = reinterpret_cast<fn_LZ4_decompress_generic>(lz4GenAddr);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        int hooked = 0;
        if (s_clusterCount > 0) {
            hooked += DetourAttachHook(reinterpret_cast<void**>(&s_origInflate),
                reinterpret_cast<void*>(&Hook_inflate), "inflate");
            hooked += DetourAttachHook(reinterpret_cast<void**>(&s_origInflateEnd),
                reinterpret_cast<void*>(&Hook_inflateEnd), "inflateEnd");
            hooked += DetourAttachHook(reinterpret_cast<void**>(&s_origInflateInit),
                reinterpret_cast<void*>(&Hook_inflateInit), "inflateInit2");
            hooked += DetourAttachHook(reinterpret_cast<void**>(&s_origInflateReset),
                reinterpret_cast<void*>(&Hook_inflateReset), "inflateReset");
        }

        if (s_origLZ4) {
            s_lz4Hooked = DetourAttachHook(reinterpret_cast<void**>(&s_origLZ4),
                reinterpret_cast<void*>(&Hook_LZ4_decompress_safe), "LZ4_decompress_safe");
            hooked += s_lz4Hooked;
        }

        if (s_origLZ4Generic) {
            s_lz4GenericHooked = DetourAttachHook(reinterpret_cast<void**>(&s_origLZ4Generic),
                reinterpret_cast<void*>(&Hook_LZ4_decompress_generic), "LZ4_decompress_generic");
            hooked += s_lz4GenericHooked;
        }

        LONG err = DetourTransactionCommit();
        if (err != NO_ERROR) {
            logger::error("DetourTransactionCommit failed: error {}", err);
        }

        // Hook extra VR inflate clusters (cluster 2+) via additional transactions
        for (int i = 1; i < s_clusterCount; i++) {
            auto& c = s_clusters[i];
            auto* origInf   = reinterpret_cast<void*>(c.inflate);
            auto* origEnd   = reinterpret_cast<void*>(c.inflateEnd);
            auto* origInit  = reinterpret_cast<void*>(c.inflateInit2);
            auto* origReset = reinterpret_cast<void*>(c.inflateReset);

            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&origInf,   reinterpret_cast<void*>(&Hook_inflate));
            DetourAttach(&origEnd,   reinterpret_cast<void*>(&Hook_inflateEnd));
            DetourAttach(&origInit,  reinterpret_cast<void*>(&Hook_inflateInit));
            DetourAttach(&origReset, reinterpret_cast<void*>(&Hook_inflateReset));
            DetourTransactionCommit();
            logger::info("inflate cluster {} hooked via Detours", i + 1);
        }

        if (!s_lz4Hooked)
            logger::warn("LZ4_decompress_safe NOT hooked");
        if (!s_lz4GenericHooked)
            logger::warn("LZ4_decompress_generic NOT hooked — BSA frame decompression unoptimized");

        logger::info("hooks installed: {} total, clusters={} lz4={} lz4frame={}",
            hooked, s_clusterCount, s_lz4Hooked ? "yes" : "no",
            s_lz4GenericHooked ? "yes" : "no");

        // Stats logging thread: only spawn if stats are enabled
        if (s_enableStats) {
            std::thread([]() {
                int intervals[] = {5, 5, 5, 5, 5, 5, 15, 15};
                for (int s : intervals) {
                    std::this_thread::sleep_for(std::chrono::seconds(s));
                    if (s_stopLogging.load(std::memory_order_relaxed)) return;
                    LogStats();
                }
                while (!s_stopLogging.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::seconds(60));
                    if (s_stopLogging.load(std::memory_order_relaxed)) return;
                    LogStats();
                }
            }).detach();
        }
    }
}
