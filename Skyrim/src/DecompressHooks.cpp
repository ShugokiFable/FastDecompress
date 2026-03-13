#include "PCH.h"
#include "DecompressHooks.h"

#include <libdeflate.h>
#include <zlib.h>  // zlib-ng compat mode — standard zlib API, SIMD-optimized
#include <lz4.h>   // LZ4 v1.10.0
#include <immintrin.h> // AVX2 + prefetch intrinsics

namespace FastDecompress
{
    // ========================================================================
    // Build mode: set to true for baseline (passthrough) measurements,
    //             false for optimized (zlib-ng + libdeflate + LZ4 v1.10).
    // ========================================================================

    static constexpr const char* kVersion = "1.0.0";
    static constexpr bool BASELINE_MODE = false;

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
    static Snapshot s_prev{};

    void LogStats()
    {
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
    // SE: inflateInit_ (strm, version, stream_size) — 3 params
    // VR: inflateInit2_ (strm, windowBits, version, stream_size) — 4 params
    // Use generic 4-param type; SE callers just leave R9 unset (ignored by 3-param original).
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

    // ========================================================================
    // Instruction length decoder + trampoline creation
    // ========================================================================

    static std::size_t InsnLength(const std::uint8_t* p)
    {
        std::uint8_t op = p[0];
        bool rex = (op >= 0x40 && op <= 0x4F);
        std::uint8_t primary = rex ? p[1] : op;
        std::size_t base = rex ? 2 : 1;

        switch (primary) {
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x90: case 0xC3: case 0xCC:
            return base;
        case 0x85: case 0x89: case 0x8B: case 0x8D:
        case 0x33: case 0x3B: case 0x2B: {
            std::uint8_t modrm = p[base];
            std::uint8_t mod = modrm >> 6;
            std::uint8_t rm = modrm & 7;
            std::size_t len = base + 1;
            if (rm == 4 && mod != 3) len++;
            if (mod == 1) len++;
            else if (mod == 2 || (mod == 0 && rm == 5)) len += 4;
            return len;
        }
        case 0x83: {
            std::uint8_t modrm = p[base];
            std::uint8_t mod = modrm >> 6;
            std::uint8_t rm = modrm & 7;
            std::size_t len = base + 1 + 1;
            if (rm == 4 && mod != 3) len++;
            if (mod == 1) len++;
            else if (mod == 2 || (mod == 0 && rm == 5)) len += 4;
            return len;
        }
        case 0x81: case 0xC7: {
            // 0x81: op r/m, imm32; 0xC7: mov r/m, imm32
            std::uint8_t modrm = p[base];
            std::uint8_t mod = modrm >> 6;
            std::uint8_t rm = modrm & 7;
            std::size_t len = base + 1 + 4; // modrm + imm32
            if (rm == 4 && mod != 3) len++; // SIB
            if (mod == 1) len++;             // disp8
            else if (mod == 2 || (mod == 0 && rm == 5)) len += 4; // disp32
            return len;
        }
        case 0x74: case 0x75:
            return base + 1;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            // mov r32, imm32 (5 bytes) or REX.W mov r64, imm64 (10 bytes)
            return base + ((rex && (op & 0x08)) ? 8 : 4);
        case 0xE8:
            return base + 4; // call rel32
        case 0x0F: {
            // Two-byte opcodes
            std::uint8_t op2 = p[base];
            if (op2 >= 0x80 && op2 <= 0x8F)
                return base + 1 + 4; // Jcc rel32 (near conditional jumps)
            return 0;
        }
        default:
            return 0;
        }
    }

    static std::size_t FindCopySize(std::uintptr_t func, std::size_t minBytes)
    {
        const auto* p = reinterpret_cast<const std::uint8_t*>(func);
        std::size_t offset = 0;
        while (offset < minBytes) {
            auto len = InsnLength(p + offset);
            if (len == 0) return 0;
            offset += len;
        }
        return offset;
    }

    static void* CreateTrampoline(std::uintptr_t origFunc, std::size_t overwriteSize)
    {
        auto copySize = FindCopySize(origFunc, overwriteSize);
        if (copySize == 0) {
            logger::warn("trampoline: failed to decode instructions at {:x}", origFunc);
            return nullptr;
        }
        auto& tramp = SKSE::GetTrampoline();
        auto* mem = tramp.allocate(copySize + 14);
        std::memcpy(mem, reinterpret_cast<const void*>(origFunc), copySize);

        // Fix up relative calls (E8) in the copied prologue
        auto* copied = reinterpret_cast<std::uint8_t*>(mem);
        for (std::size_t pos = 0; pos < copySize; ) {
            auto len = InsnLength(copied + pos);
            if (len == 0) break;
            std::uint8_t primary = copied[pos];
            bool isRex = (primary >= 0x40 && primary <= 0x4F);
            std::uint8_t op = isRex ? copied[pos + 1] : primary;
            // Fix up instructions with rel32 offsets
            bool needsFixup = false;
            std::size_t dispOff = 0;
            if (op == 0xE8) { // CALL rel32
                needsFixup = true;
                dispOff = pos + (isRex ? 2 : 1);
            } else if (op == 0x0F && (copied[pos + (isRex ? 2 : 1)] & 0xF0) == 0x80) { // Jcc rel32
                needsFixup = true;
                dispOff = pos + (isRex ? 3 : 2);
            }
            if (needsFixup) {
                std::int32_t oldRel;
                std::memcpy(&oldRel, copied + dispOff, 4);
                auto absTarget = origFunc + pos + len + oldRel;
                auto newBase = reinterpret_cast<std::uintptr_t>(copied) + pos + len;
                auto newRel = static_cast<std::int32_t>(absTarget - newBase);
                std::memcpy(copied + dispOff, &newRel, 4);
            }
            pos += len;
        }

        auto* tail = reinterpret_cast<std::uint8_t*>(mem) + copySize;
        tail[0] = 0xFF; tail[1] = 0x25;
        std::uint32_t zero = 0;
        std::memcpy(&tail[2], &zero, 4);
        auto resumeAddr = origFunc + copySize;
        std::memcpy(&tail[6], &resumeAddr, 8);
        return mem;
    }

    // ========================================================================
    // Form decompression hooks (libdeflate vs original passthrough)
    // ========================================================================

    // SE: inflateInit_(strm, version, stream_size) — 3 params
    // VR: inflateInit2_(strm, windowBits, version, stream_size) — 4 params
    // Accept 4 params; SE callers leave p4 unset (harmless on x64).
    static int __cdecl Hook_FormInflateInit(z_streamp stream,
        std::uintptr_t p2, std::uintptr_t p3, int p4)
    {
        if constexpr (BASELINE_MODE) {
            return s_origInflateInit(stream, p2, p3, p4);
        } else {
            int ret = inflateInit2_(stream, 15, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
            if (ret == ZR_OK) stream->reserved = kZlibNgMagic;
            return ret;
        }
    }

    static int __cdecl Hook_FormInflate(z_streamp stream, int flush)
    {
        auto t0 = TrackCallTime();

        if constexpr (BASELINE_MODE) {
            int ret = s_origInflate(stream, flush);
            auto dt = QpcNow() - t0;
            s_formStats.ticks.fetch_add(dt, std::memory_order_relaxed);
            if (ret == ZR_STREAM_END || ret == ZR_OK) {
                s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                s_formStats.bytesOut.fetch_add(stream->total_out, std::memory_order_relaxed);
            }
            return ret;
        } else {
            // Optimized: try libdeflate whole-buffer first
            if (stream->total_in == 0 && stream->total_out == 0) {
                std::size_t outBytes = 0;
                thread_local auto* tl_decompressor = libdeflate_alloc_decompressor();
                auto result = libdeflate_zlib_decompress(
                    tl_decompressor,
                    stream->next_in, stream->avail_in,
                    stream->next_out, stream->avail_out,
                    &outBytes);

                if (result == LIBDEFLATE_SUCCESS) {
                    auto consumed = stream->avail_in;
                    stream->next_in += consumed;
                    stream->avail_in = 0;
                    stream->next_out += outBytes;
                    stream->avail_out -= static_cast<uInt>(outBytes);
                    stream->total_in = consumed;
                    stream->total_out = static_cast<uLong>(outBytes);

                    auto dt = QpcNow() - t0;
                    s_formStats.ticks.fetch_add(dt, std::memory_order_relaxed);
                    s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                    s_formStats.bytesOut.fetch_add(outBytes, std::memory_order_relaxed);
                    return ZR_STREAM_END;
                }
            }

            // fallback to zlib-ng streaming
            int ret = inflate(stream, flush);
            auto dt = QpcNow() - t0;
            s_formStats.ticks.fetch_add(dt, std::memory_order_relaxed);
            if (ret == ZR_STREAM_END || ret == ZR_OK) {
                s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                s_formStats.bytesOut.fetch_add(stream->total_out, std::memory_order_relaxed);
            }
            return ret;
        }
    }

    // ========================================================================
    // Global zlib hooks (zlib-ng vs original passthrough)
    //
    // Skyrim's zlib uses inflateInit_ (3 params, hardcoded wbits=15)
    // and inflateReset (1 param). NOT inflateInit2_ (4 params).
    // ========================================================================

    // Global inflateInit hook — 4 params to handle both SE and VR.
    // SE callers pass (strm, version, stream_size); VR callers pass (strm, wbits, version, stream_size).
    // In optimized mode all params are ignored; in baseline mode they're forwarded as-is.
    static int __cdecl Hook_inflateInit(z_streamp strm,
        std::uintptr_t p2, std::uintptr_t p3, int p4)
    {
        if constexpr (BASELINE_MODE) {
            return s_origInflateInit(strm, p2, p3, p4);
        } else {
            int ret = inflateInit2_(strm, 15, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
            if (ret == ZR_OK) strm->reserved = kZlibNgMagic;
            return ret;
        }
    }

    static int __cdecl Hook_inflate(z_streamp strm, int flush)
    {
        auto t0 = TrackCallTime();
        uint32_t outBefore = strm->avail_out;

        int ret;
        if constexpr (BASELINE_MODE) {
            ret = s_origInflate(strm, flush);
        } else {
            // Auto-detect one-shot (form) decompression: try libdeflate on first call.
            // This catches form inflate on VR where indirect calls bypass the E8 scanner.
            if (strm->total_in == 0 && strm->total_out == 0 && strm->reserved == kZlibNgMagic) {
                std::size_t outBytes = 0;
                thread_local auto* tl_decompressor = libdeflate_alloc_decompressor();
                auto result = libdeflate_zlib_decompress(
                    tl_decompressor,
                    strm->next_in, strm->avail_in,
                    strm->next_out, strm->avail_out,
                    &outBytes);

                if (result == LIBDEFLATE_SUCCESS) {
                    auto consumed = strm->avail_in;
                    strm->next_in += consumed;
                    strm->avail_in = 0;
                    strm->next_out += outBytes;
                    strm->avail_out -= static_cast<uInt>(outBytes);
                    strm->total_in = consumed;
                    strm->total_out = static_cast<uLong>(outBytes);

                    auto dt = QpcNow() - t0;
                    s_formStats.ticks.fetch_add(dt, std::memory_order_relaxed);
                    s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
                    s_formStats.bytesOut.fetch_add(outBytes, std::memory_order_relaxed);
                    return ZR_STREAM_END;
                }
            }

            // Streaming path: zlib-ng for our streams, original zlib for pre-hook streams
            if (strm->reserved == kZlibNgMagic) {
                ret = inflate(strm, flush);
            } else {
                s_stockZlibCalls.fetch_add(1, std::memory_order_relaxed);
                ret = s_origInflate(strm, flush);
            }
        }

        auto dt = QpcNow() - t0;
        s_streamStats.ticks.fetch_add(dt, std::memory_order_relaxed);
        s_streamStats.calls.fetch_add(1, std::memory_order_relaxed);
        s_streamStats.bytesOut.fetch_add(outBefore - strm->avail_out, std::memory_order_relaxed);
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
        PrefetchSource(src, compressedSize);
        auto t0 = TrackCallTime();

        int ret;
        if constexpr (BASELINE_MODE) {
            ret = s_origLZ4(src, dst, compressedSize, dstCapacity);
        } else {
            ret = LZ4_decompress_safe(src, dst, compressedSize, dstCapacity);
        }

        auto dt = QpcNow() - t0;
        s_lz4Stats.ticks.fetch_add(dt, std::memory_order_relaxed);
        s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
        if (ret > 0)
            s_lz4Stats.bytesOut.fetch_add(ret, std::memory_order_relaxed);
        return ret;
    }

    // LZ4_decompress_generic: the 6-param internal function called by
    // BSResource::CompressedArchiveStream for LZ4 frame block decompression.
    // This is the main BSA decompression path — handles the bulk of game data.
    static int __cdecl Hook_LZ4_decompress_generic(const char* src, char* dst,
        int srcSize, int outputSize, const char* dictStart, int dictSize)
    {
        PrefetchSource(src, srcSize);
        auto t0 = TrackCallTime();

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

        auto dt = QpcNow() - t0;
        s_lz4Stats.ticks.fetch_add(dt, std::memory_order_relaxed);
        s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
        if (ret > 0)
            s_lz4Stats.bytesOut.fetch_add(ret, std::memory_order_relaxed);
        if (dictSize > 0)
            s_lz4DictCalls.fetch_add(1, std::memory_order_relaxed);
        else
            s_lz4NoDictCalls.fetch_add(1, std::memory_order_relaxed);
        return ret;
    }

    // ========================================================================
    // Hook helpers
    // ========================================================================

    static void WriteAbsoluteJump(std::uintptr_t targetAddr, void* newFunc)
    {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(targetAddr), 12, PAGE_EXECUTE_READWRITE, &oldProtect);
        std::uint8_t code[12];
        code[0] = 0x48; code[1] = 0xB8;
        std::memcpy(&code[2], &newFunc, 8);
        code[10] = 0xFF; code[11] = 0xE0;
        std::memcpy(reinterpret_cast<void*>(targetAddr), code, 12);
        VirtualProtect(reinterpret_cast<void*>(targetAddr), 12, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(targetAddr), 12);
    }

    static void DetourCall(std::uintptr_t callAddr, void* newFunc)
    {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(callAddr), 5, PAGE_EXECUTE_READWRITE, &oldProtect);
        auto target = reinterpret_cast<std::uintptr_t>(newFunc);
        auto rel = static_cast<std::int32_t>(target - (callAddr + 5));
        if (static_cast<std::int64_t>(target - (callAddr + 5)) != static_cast<std::int64_t>(rel)) {
            auto& trampoline = SKSE::GetTrampoline();
            auto* trampAddr = trampoline.allocate(14);
            std::uint8_t jmp[14];
            jmp[0] = 0xFF; jmp[1] = 0x25;
            std::uint32_t zero = 0;
            std::memcpy(&jmp[2], &zero, 4);
            std::memcpy(&jmp[6], &target, 8);
            std::memcpy(trampAddr, jmp, 14);
            target = reinterpret_cast<std::uintptr_t>(trampAddr);
            rel = static_cast<std::int32_t>(target - (callAddr + 5));
        }
        std::uint8_t call[5];
        call[0] = 0xE8;
        std::memcpy(&call[1], &rel, 4);
        std::memcpy(reinterpret_cast<void*>(callAddr), call, 5);
        VirtualProtect(reinterpret_cast<void*>(callAddr), 5, oldProtect, &oldProtect);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(callAddr), 5);
    }

    // ========================================================================
    // Signature scanning
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

    // ========================================================================
    // Signatures & offset tables
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
    // VR/FO4: mov [rsp+18h],rbx; push rsi; sub rsp,20h
    static constexpr std::uint8_t kSig_inflateInit2_v1[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20,
    };
    // AE 1.6.1170: REX push rbx; sub rsp,20h; mov rbx,rcx; test rdx,rdx
    // (Ghidra-verified real inflateInit2_ at offset +0x1A90)
    static constexpr std::uint8_t kSig_inflateInit2_AE[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xD2,
    };

    // --- inflateReset signatures per version ---
    // VR/FO4: test rcx,rcx; jz xx; mov rax,[rcx+28h]
    static constexpr std::uint8_t kSig_inflateReset_VR[] = {
        0x48, 0x85, 0xC9,
    };
    // AE 1.6.1170: mov rax,rcx; test rcx,rcx
    // (Ghidra-verified real inflateResetKeep at offset +0x1C70)
    static constexpr std::uint8_t kSig_inflateReset_AE[] = {
        0x48, 0x8B, 0xC1, 0x48, 0x85, 0xC9,
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

    // Cluster 1 offset tables (inflate starts with kSig_inflatePrefix)
    static constexpr InflateOffsets kOffsets_AE1170 = {
        0x18A0, 0x1A90, 0x1C70,
        kSig_inflateInit2_AE, sizeof(kSig_inflateInit2_AE),
        kSig_inflateReset_AE, sizeof(kSig_inflateReset_AE)
    };
    static constexpr InflateOffsets kOffsets_VR = {
        0x19E0, 0x1A70, 0x1C30,
        kSig_inflateInit2_v1, sizeof(kSig_inflateInit2_v1),
        kSig_inflateReset_VR, sizeof(kSig_inflateReset_VR)
    };
    static constexpr InflateOffsets kAllOffsets_c1[] = { kOffsets_AE1170, kOffsets_VR };

    // Cluster 2 offset tables (inflate starts with kSig_inflate_v2)
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
    // Prologue: mov [rsp+18h],rbx; push rbp..r15; lea rbp,[rsp-17h]; sub rsp,0E0h
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

    static bool IsInflateAddr(std::uintptr_t addr)
    {
        for (int i = 0; i < s_clusterCount; i++)
            if (s_clusters[i].inflate == addr) return true;
        return false;
    }

    static bool IsInflateInit2Addr(std::uintptr_t addr)
    {
        for (int i = 0; i < s_clusterCount; i++)
            if (addr == s_clusters[i].inflateInit2) return true;
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

    void Install()
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_qpcFreq = freq.QuadPart;
        s_installTime = QpcNow();

        const bool isVR = REL::Module::IsVR();
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
        // Step 1: Locate zlib inflate cluster
        // SE/AE: Address Library (works across all versions)
        // VR: Signature scanning (no Address Library, version is frozen)
        // ============================================================

        if (!isVR) {
            // SE/AE: Use Address Library IDs — guaranteed correct on any version
            s_clusters[0] = {
                kID_inflate.address(),
                kID_inflateEnd.address(),
                kID_inflateInit2.address(),
                kID_inflateReset.address()
            };
            s_clusterCount = 1;
            auto& c = s_clusters[0];
            logger::info("inflate cluster 1 via AddressLib at {:x} (end={:x} init2={:x} reset={:x})",
                c.inflate, c.inflateEnd, c.inflateInit2, c.inflateReset);
        } else {
            // VR: Signature scanning
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

        // Step 2: Create trampolines from the first cluster
        if (s_clusterCount > 0) {
            auto& c = s_clusters[0];
            s_origInflate = reinterpret_cast<fn_inflate>(CreateTrampoline(c.inflate, 12));
            s_origInflateEnd = reinterpret_cast<fn_inflateEnd>(CreateTrampoline(c.inflateEnd, 12));
            s_origInflateReset = reinterpret_cast<fn_inflateReset>(CreateTrampoline(c.inflateReset, 12));
            s_origInflateInit = reinterpret_cast<fn_inflateInit>(CreateTrampoline(c.inflateInit2, 12));

            if (!s_origInflate || !s_origInflateEnd || !s_origInflateReset || !s_origInflateInit) {
                logger::error("failed to create zlib trampolines");
                s_clusterCount = 0;
            }
        }

        // Step 3: Form hooks — scan for CALL instructions targeting inflate
        int formHooked = 0;
        if (s_clusterCount > 0) {
            for (std::size_t i = 0; i + 5 <= textSize; i++) {
                auto callAddr = textBase + i;
                if (*reinterpret_cast<const std::uint8_t*>(callAddr) != 0xE8) continue;

                std::int32_t rel32;
                std::memcpy(&rel32, reinterpret_cast<const void*>(callAddr + 1), 4);
                auto target = static_cast<std::uintptr_t>(
                    static_cast<std::int64_t>(callAddr) + 5 + rel32);
                if (!IsInflateAddr(target)) continue;

                std::uintptr_t initCallAddr = 0;
                auto maxBack = std::min<std::size_t>(i, 0x200);
                for (std::size_t j = 5; j <= maxBack; j++) {
                    auto prevAddr = callAddr - j;
                    if (*reinterpret_cast<const std::uint8_t*>(prevAddr) != 0xE8) continue;
                    std::int32_t prevRel;
                    std::memcpy(&prevRel, reinterpret_cast<const void*>(prevAddr + 1), 4);
                    auto prevTarget = static_cast<std::uintptr_t>(
                        static_cast<std::int64_t>(prevAddr) + 5 + prevRel);
                    if (IsInflateInit2Addr(prevTarget)) {
                        initCallAddr = prevAddr;
                        break;
                    }
                }
                if (!initCallAddr) continue;

                DetourCall(initCallAddr, reinterpret_cast<void*>(&Hook_FormInflateInit));
                DetourCall(callAddr, reinterpret_cast<void*>(&Hook_FormInflate));
                formHooked++;
            }
        }

        // Step 4: Global zlib hooks — overwrite all cluster functions
        for (int i = 0; i < s_clusterCount; i++) {
            auto& c = s_clusters[i];
            WriteAbsoluteJump(c.inflate, reinterpret_cast<void*>(&Hook_inflate));
            WriteAbsoluteJump(c.inflateEnd, reinterpret_cast<void*>(&Hook_inflateEnd));
            WriteAbsoluteJump(c.inflateInit2, reinterpret_cast<void*>(&Hook_inflateInit));
            WriteAbsoluteJump(c.inflateReset, reinterpret_cast<void*>(&Hook_inflateReset));
        }

        // Step 5: LZ4_decompress_safe (sig scan — not in Address Library, 0 calls observed)
        {
            std::size_t scanPos = 0;
            while (true) {
                auto lz4Addr = ScanForBytes(textBase, textSize,
                    kSig_LZ4_decompress_safe, sizeof(kSig_LZ4_decompress_safe), &scanPos);
                if (!lz4Addr) break;

                s_origLZ4 = reinterpret_cast<fn_LZ4_decompress_safe>(
                    CreateTrampoline(lz4Addr, 12));
                if (!s_origLZ4) {
                    logger::error("failed to create LZ4 trampoline");
                    break;
                }

                WriteAbsoluteJump(lz4Addr, reinterpret_cast<void*>(&Hook_LZ4_decompress_safe));
                s_lz4Hooked = true;
                logger::info("LZ4 hooked at {:x}", lz4Addr);
                break;
            }
            if (!s_lz4Hooked)
                logger::warn("LZ4 NOT hooked");
        }

        // Step 6: LZ4_decompress_generic (BSA frame block decompressor)
        // SE/AE: Address Library; VR: Signature scanning
        {
            std::uintptr_t lz4GenAddr = 0;
            if (!isVR) {
                lz4GenAddr = kID_LZ4_decompress_generic.address();
                logger::info("LZ4_decompress_generic via AddressLib at {:x}", lz4GenAddr);
            } else {
                std::size_t scanPos = 0;
                lz4GenAddr = ScanForBytes(textBase, textSize,
                    kSig_LZ4_decompress_generic, sizeof(kSig_LZ4_decompress_generic), &scanPos);
            }

            if (lz4GenAddr) {
                s_origLZ4Generic = reinterpret_cast<fn_LZ4_decompress_generic>(
                    CreateTrampoline(lz4GenAddr, 12));
                if (s_origLZ4Generic) {
                    WriteAbsoluteJump(lz4GenAddr, reinterpret_cast<void*>(&Hook_LZ4_decompress_generic));
                    s_lz4GenericHooked = true;
                    logger::info("LZ4_decompress_generic hooked at {:x}", lz4GenAddr);
                } else {
                    logger::error("failed to create LZ4_decompress_generic trampoline");
                }
            }
            if (!s_lz4GenericHooked)
                logger::warn("LZ4_decompress_generic NOT hooked — BSA frame decompression unoptimized");
        }

        logger::info("hooks installed: clusters={} formCallSites={} lz4={} lz4frame={}",
            s_clusterCount, formHooked, s_lz4Hooked ? "yes" : "no",
            s_lz4GenericHooked ? "yes" : "no");
        if (formHooked == 0 && s_clusterCount > 0) {
            logger::info("  (no form CALL detours — form decompression still optimized via global inflate hook)");
        }

        // Stats logging thread: frequent early (loading phase), then every 60s
        // 5s, 10s, 15s, 20s, 25s, 30s, 45s, 60s, then every 60s
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
