#include "PCH.h"
#include "DecompressHooks.h"

#include <libdeflate.h>
#include <lz4.h>
#include <zlib.h>  // zlib-ng compat mode — standard zlib API, SIMD-optimized

namespace FastDecompress
{
    // ========================================================================
    // Compile-time toggle: compat (direct passthrough) vs wrapper mode
    //
    // COMPAT_DIRECT = true:  pass game's z_stream straight to zlib-ng compat
    //                        inflate (ABI-compatible, no field copying)
    // COMPAT_DIRECT = false: allocate separate z_stream, copy fields in/out
    //                        (simulates the overhead of a wrapper approach)
    // ========================================================================
    static constexpr bool COMPAT_DIRECT = true;

    // ========================================================================
    // Instrumentation — tracks call counts, bytes, and cumulative time
    // ========================================================================

    struct Stats {
        std::atomic<uint64_t> calls{0};
        std::atomic<uint64_t> bytesIn{0};
        std::atomic<uint64_t> bytesOut{0};
        std::atomic<int64_t>  ticksTotal{0};  // QPC ticks
    };

    static Stats s_formStats;     // libdeflate (DecompressCurrentForm)
    static Stats s_streamStats;   // zlib-ng (streaming inflate)
    static Stats s_lz4Stats;      // LZ4 (inline inflate)
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

    void LogStats()
    {
        auto elapsed = TicksToMs(QpcNow() - s_installTime);

        auto fc = s_formStats.calls.load();
        auto fbi = s_formStats.bytesIn.load();
        auto fbo = s_formStats.bytesOut.load();
        auto ft = TicksToMs(s_formStats.ticksTotal.load());

        auto sc = s_streamStats.calls.load();
        auto sbi = s_streamStats.bytesIn.load();
        auto sbo = s_streamStats.bytesOut.load();
        auto st = TicksToMs(s_streamStats.ticksTotal.load());

        auto lc = s_lz4Stats.calls.load();
        auto lbi = s_lz4Stats.bytesIn.load();
        auto lbo = s_lz4Stats.bytesOut.load();
        auto lt = TicksToMs(s_lz4Stats.ticksTotal.load());

        logger::info("=== FastDecompress Stats ({:.1f}s since install) ===", elapsed / 1000.0);
        logger::info("  libdeflate (form): {} calls, {:.1f} MB in -> {:.1f} MB out, {:.1f} ms total",
            fc, fbi / (1024.0 * 1024.0), fbo / (1024.0 * 1024.0), ft);
        logger::info("  zlib-ng {} (stream):  {} calls, {:.1f} MB in -> {:.1f} MB out, {:.1f} ms total",
            COMPAT_DIRECT ? "direct" : "wrapper",
            sc, sbi / (1024.0 * 1024.0), sbo / (1024.0 * 1024.0), st);
        if (lc > 0) {
            logger::info("  LZ4 (inline): {} calls, {:.1f} MB in -> {:.1f} MB out, {:.1f} ms total",
                lc, lbi / (1024.0 * 1024.0), lbo / (1024.0 * 1024.0), lt);
            if (lbo > 0)
                logger::info("  LZ4 throughput: {:.0f} MB/s", lt > 0 ? ((lbo / (1024.0 * 1024.0)) / (lt / 1000.0)) : 0);
        }

        double totalMs = ft + st + lt;
        double totalMB = (fbo + sbo + lbo) / (1024.0 * 1024.0);
        logger::info("  Combined: {:.1f} ms decompression time, {:.1f} MB decompressed",
            totalMs, totalMB);

        if (totalMB > 0) {
            logger::info("  Throughput: {:.0f} MB/s", totalMs > 0 ? (totalMB / (totalMs / 1000.0)) : 0);
        }
    }

    // ========================================================================
    // Game's z_stream layout (zlib 1.2.7, x64 Windows)
    //
    // In compat mode, zlib-ng's z_stream uses the same layout:
    //   unsigned long = 4 bytes on Windows x64, so total_in/total_out match.
    // This means we can pass the game's z_stream directly to zlib-ng compat.
    // ========================================================================

    // zlib return codes — use local constants to avoid macro expansion issues
    static constexpr int ZR_OK = 0;
    static constexpr int ZR_STREAM_END = 1;
    static constexpr int ZR_STREAM_ERROR = -2;
    static constexpr int ZR_BUF_ERROR = -5;

    // ========================================================================
    // LZ4 block detection & decompression
    //
    // 12-byte header: [4 bytes: "\x04LZ4"] [4 bytes LE: uncompressed_size]
    //                 [4 bytes LE: lz4_data_size] [N bytes: LZ4 block]
    // BA2 packedSize = 12 + lz4_data_size
    // ========================================================================

    static constexpr uint8_t kLZ4Magic[4] = { 0x04, 'L', 'Z', '4' };
    static constexpr uint32_t kLZ4HeaderSize = 12;

    static std::atomic<int> s_lz4Count{0};

    // ========================================================================
    // LZ4 inline decompression for inflate hook
    //
    // When inflate sees LZ4 magic on the first call, AND all input + output
    // fit in the current buffers, decompress inline and return Z_STREAM_END.
    // No state/context needed — single-call decompression.
    // ========================================================================

    // Try whole-buffer LZ4 decompression (for form inflate where all data fits).
    // Does NOT touch strm on failure — safe to fall through to zlib.
    static bool TryLZ4WholeBuffer(z_streamp strm)
    {
        if (strm->avail_in < kLZ4HeaderSize)
            return false;
        if (std::memcmp(strm->next_in, kLZ4Magic, 4) != 0)
            return false;

        uint32_t uncompSize, lz4DataSize;
        std::memcpy(&uncompSize, strm->next_in + 4, 4);
        std::memcpy(&lz4DataSize, strm->next_in + 8, 4);
        uint32_t totalSize = kLZ4HeaderSize + lz4DataSize;

        // Only handle if ALL input and output space is available
        if (strm->avail_in < totalSize)
            return false;
        if (strm->avail_out < uncompSize)
            return false;

        int ret = LZ4_decompress_safe(
            reinterpret_cast<const char*>(strm->next_in + kLZ4HeaderSize),
            reinterpret_cast<char*>(strm->next_out),
            static_cast<int>(lz4DataSize),
            static_cast<int>(uncompSize));

        if (ret <= 0)
            return false;

        uint32_t outSize = static_cast<uint32_t>(ret);
        strm->next_in += totalSize;
        strm->avail_in -= totalSize;
        strm->total_in += totalSize;
        strm->next_out += outSize;
        strm->avail_out -= outSize;
        strm->total_out += outSize;

        int cnt = ++s_lz4Count;
        if (cnt <= 50 || cnt % 5000 == 0)
            logger::info("LZ4 form #{}: in={} out={}", cnt, totalSize, outSize);

        return true;
    }

    // ========================================================================
    // LZ4 output buffering (map-based, never touches strm->state)
    //
    // When all compressed input fits in a single inflate call (< 64KB) but
    // the output is too large for the game's output buffer (> 128KB), we
    // decompress into an internal buffer and deliver output in chunks.
    // Every inflate call produces output — no "consume input, no output" issue.
    //
    // Uses a global map keyed by z_streamp to track output buffers.
    // The zlib-ng state in strm->state is never modified — it stays alive
    // and is freed normally by the game's inflateEnd call.
    // ========================================================================

    struct LZ4OutBuf {
        int64_t  t0;
        uint32_t uncompSize;
        uint32_t totalCompSize;
        uint32_t decompOffset;    // bytes of decompressed output delivered so far
        std::vector<uint8_t> decompBuf;
    };

    static std::mutex s_outBufMtx;
    static std::unordered_map<z_streamp, LZ4OutBuf*> s_outBufMap;

    static LZ4OutBuf* FindOutBuf(z_streamp strm) {
        std::lock_guard<std::mutex> lock(s_outBufMtx);
        auto it = s_outBufMap.find(strm);
        return (it != s_outBufMap.end()) ? it->second : nullptr;
    }

    static void RemoveOutBuf(z_streamp strm) {
        std::lock_guard<std::mutex> lock(s_outBufMtx);
        auto it = s_outBufMap.find(strm);
        if (it != s_outBufMap.end()) {
            delete it->second;
            s_outBufMap.erase(it);
        }
    }

    // Deliver buffered decompressed data in chunks. Always produces output.
    // IMPORTANT: Input is NOT consumed until the final chunk is delivered.
    // This matches normal zlib behavior where avail_in > 0 as long as there's
    // pending output. The game's inflate loop checks avail_in to decide whether
    // to keep calling inflate — if we set avail_in=0 early, the game stops
    // the loop and uses incomplete output.
    static int DrainLZ4OutBuf(z_streamp strm, LZ4OutBuf* ob)
    {
        uint32_t remaining = ob->uncompSize - ob->decompOffset;
        uint32_t give = (strm->avail_out < remaining) ? strm->avail_out : remaining;

        if (give > 0) {
            std::memcpy(strm->next_out, ob->decompBuf.data() + ob->decompOffset, give);
            ob->decompOffset += give;
            strm->next_out += give;
            strm->avail_out -= give;
            strm->total_out += give;
        }

        if (ob->decompOffset >= ob->uncompSize) {
            // All output delivered — NOW consume all input
            strm->total_in += ob->totalCompSize;
            strm->avail_in = 0;

            // Record stats and clean up
            auto dt = QpcNow() - ob->t0;
            s_lz4Stats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
            s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
            s_lz4Stats.bytesIn.fetch_add(ob->totalCompSize, std::memory_order_relaxed);
            s_lz4Stats.bytesOut.fetch_add(ob->uncompSize, std::memory_order_relaxed);

            logger::info("LZ4 outbuf done: {} -> {} bytes ({:.2f}ms)",
                ob->totalCompSize, ob->uncompSize, TicksToMs(dt));

            RemoveOutBuf(strm);
            return ZR_STREAM_END;
        }

        return ZR_OK;
    }

    // All compressed input is available. Decompress into buffer, output first chunk.
    static int StartLZ4OutBuf(z_streamp strm)
    {
        uint32_t uncompSize, lz4DataSize;
        std::memcpy(&uncompSize, strm->next_in + 4, 4);
        std::memcpy(&lz4DataSize, strm->next_in + 8, 4);
        uint32_t totalCompSize = kLZ4HeaderSize + lz4DataSize;

        // Decompress into internal buffer
        auto* ob = new LZ4OutBuf{};
        ob->t0 = QpcNow();
        ob->uncompSize = uncompSize;
        ob->totalCompSize = totalCompSize;
        ob->decompOffset = 0;
        ob->decompBuf.resize(uncompSize);

        int ret = LZ4_decompress_safe(
            reinterpret_cast<const char*>(strm->next_in + kLZ4HeaderSize),
            reinterpret_cast<char*>(ob->decompBuf.data()),
            static_cast<int>(lz4DataSize),
            static_cast<int>(uncompSize));

        if (ret <= 0 || static_cast<uint32_t>(ret) != uncompSize) {
            logger::error("LZ4 outbuf decompress failed: ret={} expected={}", ret, uncompSize);
            delete ob;
            return Z_DATA_ERROR;
        }

        // DON'T consume input yet! Leave avail_in > 0 so the game's inflate
        // loop keeps calling inflate. Input is consumed in DrainLZ4OutBuf
        // when the final output chunk is delivered. This matches normal zlib
        // behavior where avail_in > 0 as long as there's pending output.

        // Register output buffer in global map
        {
            std::lock_guard<std::mutex> lock(s_outBufMtx);
            s_outBufMap[strm] = ob;
        }

        int cnt = ++s_lz4Count;
        logger::info("LZ4 outbuf #{}: {} -> {} bytes (avail_out={})",
            cnt, totalCompSize, uncompSize, strm->avail_out);

        // Deliver first chunk of output immediately
        return DrainLZ4OutBuf(strm, ob);
    }

    // ========================================================================
    // libdeflate — TESFile::DecompressCurrentForm (whole-buffer, ~2.4x faster)
    // ========================================================================

    static int __stdcall Hook_FormInflateInit(z_streamp stream, const char*, int)
    {
        stream->state = nullptr;
        return ZR_OK;
    }

    static int __stdcall Hook_FormInflate(z_streamp stream, int)
    {
        auto t0 = QpcNow();

        // Try LZ4 first (repacked BA2 data starts with "\x04LZ4")
        if (TryLZ4WholeBuffer(stream)) {
            auto dt = QpcNow() - t0;
            s_lz4Stats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
            s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
            s_lz4Stats.bytesIn.fetch_add(stream->total_in, std::memory_order_relaxed);
            s_lz4Stats.bytesOut.fetch_add(stream->total_out, std::memory_order_relaxed);
            return ZR_STREAM_END;
        }

        std::size_t outBytes = 0;
        auto* decompressor = libdeflate_alloc_decompressor();

        auto result = libdeflate_zlib_decompress(
            decompressor,
            stream->next_in, stream->avail_in,
            stream->next_out, stream->avail_out,
            &outBytes);
        libdeflate_free_decompressor(decompressor);

        auto dt = QpcNow() - t0;
        s_formStats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
        s_formStats.calls.fetch_add(1, std::memory_order_relaxed);
        s_formStats.bytesIn.fetch_add(stream->avail_in, std::memory_order_relaxed);
        s_formStats.bytesOut.fetch_add(outBytes, std::memory_order_relaxed);

        if (result == LIBDEFLATE_SUCCESS) {
            stream->total_in = stream->avail_in;
            stream->total_out = static_cast<uLong>(outBytes);
            return ZR_STREAM_END;
        }

        if (result == LIBDEFLATE_INSUFFICIENT_SPACE)
            return ZR_BUF_ERROR;

        return ZR_STREAM_ERROR;
    }

    // ========================================================================
    // zlib-ng compat — streaming inflate replacement
    //
    // COMPAT_DIRECT = true:  pass z_stream straight through (ABI-compatible)
    // COMPAT_DIRECT = false: allocate wrapper z_stream, copy fields each call
    // ========================================================================

    // Original function pointers (trampolines into stock zlib 1.2.7 code)
    using inflate_fn = int(__cdecl*)(z_streamp, int);
    using inflateEnd_fn = int(__cdecl*)(z_streamp);
    using inflateReset_fn = int(__cdecl*)(z_streamp);

    static inflate_fn s_origInflate = nullptr;
    static inflateEnd_fn s_origInflateEnd = nullptr;
    static inflateReset_fn s_origInflateReset = nullptr;

    // --- Wrapper mode state (only used when COMPAT_DIRECT = false) ---
    static constexpr uint32_t WRAP_MAGIC = 0x5A4E4753; // "ZNGS"

    struct WrapState {
        uint32_t magic;
        z_stream zs;
    };

    static bool IsWrapStream(void* state) {
        if (!state) return false;
        return static_cast<WrapState*>(state)->magic == WRAP_MAGIC;
    }

    // --- inflateInit2_ hook ---
    static int __cdecl Hook_inflateInit2(z_streamp strm, int windowBits,
        const char* version, int stream_size)
    {
        if constexpr (COMPAT_DIRECT) {
            // Direct: just forward to zlib-ng compat's inflateInit2_
            return inflateInit2_(strm, windowBits, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
        } else {
            // Wrapper: allocate separate z_stream
            auto* w = new WrapState{};
            w->magic = WRAP_MAGIC;
            std::memset(&w->zs, 0, sizeof(w->zs));
            int ret = inflateInit2_(&w->zs, windowBits, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
            if (ret != Z_OK) {
                delete w;
                return ret;
            }
            strm->state = reinterpret_cast<struct internal_state*>(w);
            strm->total_in = 0;
            strm->total_out = 0;
            strm->msg = nullptr;
            return Z_OK;
        }
    }

    // --- inflate hook ---
    static int __cdecl Hook_inflate(z_streamp strm, int flush)
    {
        if constexpr (COMPAT_DIRECT) {
            // Check if this stream has a pending LZ4 output buffer
            if (auto* ob = FindOutBuf(strm))
                return DrainLZ4OutBuf(strm, ob);

            // On first inflate call, try inline LZ4 decompression
            if (strm->total_in == 0 && strm->total_out == 0 &&
                strm->avail_in >= kLZ4HeaderSize &&
                std::memcmp(strm->next_in, kLZ4Magic, 4) == 0)
            {
                auto t0 = QpcNow();
                if (TryLZ4WholeBuffer(strm)) {
                    auto dt = QpcNow() - t0;
                    int cnt = ++s_lz4Count;
                    s_lz4Stats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
                    s_lz4Stats.calls.fetch_add(1, std::memory_order_relaxed);
                    s_lz4Stats.bytesIn.fetch_add(strm->total_in, std::memory_order_relaxed);
                    s_lz4Stats.bytesOut.fetch_add(strm->total_out, std::memory_order_relaxed);

                    if (cnt <= 50 || cnt % 5000 == 0)
                        logger::info("LZ4 inflate #{}: {} -> {} bytes ({:.2f}ms)",
                            cnt, strm->total_in, strm->total_out, TicksToMs(dt));

                    // Clean up zlib-ng state since we won't use it
                    inflateEnd(strm);
                    strm->state = nullptr;
                    return ZR_STREAM_END;
                }
                // TryLZ4WholeBuffer failed — output buffer too small.
                // All input should be available (repacker ensures comp < 64KB).
                // Decompress into internal buffer, deliver output in chunks.
                return StartLZ4OutBuf(strm);
            }

            // Normal zlib-ng passthrough with instrumentation
            auto t0 = QpcNow();
            uint32_t inBefore = strm->avail_in;
            uint32_t outBefore = strm->avail_out;

            int ret = inflate(strm, flush);

            auto dt = QpcNow() - t0;
            s_streamStats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
            s_streamStats.calls.fetch_add(1, std::memory_order_relaxed);
            s_streamStats.bytesIn.fetch_add(inBefore - strm->avail_in, std::memory_order_relaxed);
            s_streamStats.bytesOut.fetch_add(outBefore - strm->avail_out, std::memory_order_relaxed);
            return ret;
        } else {
            // Wrapper mode
            if (!IsWrapStream(strm->state))
                return s_origInflate(strm, flush);

            auto t0 = QpcNow();
            auto* w = reinterpret_cast<WrapState*>(strm->state);

            // Copy game stream -> wrapper
            w->zs.next_in = strm->next_in;
            w->zs.avail_in = strm->avail_in;
            w->zs.next_out = strm->next_out;
            w->zs.avail_out = strm->avail_out;

            int ret = inflate(&w->zs, flush);

            uint32_t consumed = strm->avail_in - w->zs.avail_in;
            uint32_t produced = strm->avail_out - w->zs.avail_out;

            // Copy back
            strm->next_in = w->zs.next_in;
            strm->avail_in = w->zs.avail_in;
            strm->next_out = w->zs.next_out;
            strm->avail_out = w->zs.avail_out;
            strm->total_in = w->zs.total_in;
            strm->total_out = w->zs.total_out;
            if (w->zs.msg) strm->msg = w->zs.msg;

            auto dt = QpcNow() - t0;
            s_streamStats.ticksTotal.fetch_add(dt, std::memory_order_relaxed);
            s_streamStats.calls.fetch_add(1, std::memory_order_relaxed);
            s_streamStats.bytesIn.fetch_add(consumed, std::memory_order_relaxed);
            s_streamStats.bytesOut.fetch_add(produced, std::memory_order_relaxed);
            return ret;
        }
    }

    // --- inflateEnd hook ---
    static int __cdecl Hook_inflateEnd(z_streamp strm)
    {
        if constexpr (COMPAT_DIRECT) {
            RemoveOutBuf(strm);  // clean up any pending LZ4 output buffer
            if (!strm->state) return Z_OK;
            return inflateEnd(strm);
        } else {
            if (!IsWrapStream(strm->state))
                return s_origInflateEnd(strm);
            auto* w = reinterpret_cast<WrapState*>(strm->state);
            inflateEnd(&w->zs);
            delete w;
            strm->state = nullptr;
            return Z_OK;
        }
    }

    // --- inflateReset hook ---
    static int __cdecl Hook_inflateReset(z_streamp strm)
    {
        if constexpr (COMPAT_DIRECT) {
            RemoveOutBuf(strm);  // clean up any pending LZ4 output buffer
            return inflateReset(strm);
        } else {
            if (!IsWrapStream(strm->state))
                return s_origInflateReset(strm);
            auto* w = reinterpret_cast<WrapState*>(strm->state);
            int ret = inflateReset(&w->zs);
            strm->total_in = 0;
            strm->total_out = 0;
            strm->msg = nullptr;
            return ret;
        }
    }

    // ========================================================================
    // Hook helpers
    // ========================================================================

    // 12-byte absolute jump: mov rax, imm64; jmp rax
    static void WriteAbsoluteJump(std::uintptr_t targetAddr, void* newFunc)
    {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(targetAddr), 12, PAGE_EXECUTE_READWRITE, &oldProtect);

        std::uint8_t code[12];
        code[0] = 0x48; code[1] = 0xB8;               // mov rax,
        std::memcpy(&code[2], &newFunc, 8);             //   imm64
        code[10] = 0xFF; code[11] = 0xE0;              // jmp rax
        std::memcpy(reinterpret_cast<void*>(targetAddr), code, 12);

        VirtualProtect(reinterpret_cast<void*>(targetAddr), 12, oldProtect, &oldProtect);
    }

    // Rewrite a 5-byte CALL rel32 to point at newFunc (uses trampoline if >2GB away)
    static void DetourCall(std::uintptr_t callAddr, void* newFunc)
    {
        DWORD oldProtect;
        VirtualProtect(reinterpret_cast<void*>(callAddr), 5, PAGE_EXECUTE_READWRITE, &oldProtect);

        auto target = reinterpret_cast<std::uintptr_t>(newFunc);
        auto rel = static_cast<std::int32_t>(target - (callAddr + 5));

        if (static_cast<std::int64_t>(target - (callAddr + 5)) != static_cast<std::int64_t>(rel)) {
            auto& trampoline = F4SE::GetTrampoline();
            auto* trampAddr = trampoline.allocate(14);
            std::uint8_t jmp[14];
            jmp[0] = 0xFF; jmp[1] = 0x25;              // jmp [rip+0]
            std::uint32_t zero = 0;
            std::memcpy(&jmp[2], &zero, 4);
            std::memcpy(&jmp[6], &target, 8);           // absolute address
            std::memcpy(trampAddr, jmp, 14);
            target = reinterpret_cast<std::uintptr_t>(trampAddr);
            rel = static_cast<std::int32_t>(target - (callAddr + 5));
        }

        std::uint8_t call[5];
        call[0] = 0xE8;
        std::memcpy(&call[1], &rel, 4);
        std::memcpy(reinterpret_cast<void*>(callAddr), call, 5);

        VirtualProtect(reinterpret_cast<void*>(callAddr), 5, oldProtect, &oldProtect);
    }

    // Create a trampoline that executes the original function's first N bytes,
    // then jumps back to originalAddr + N.
    static void* CreateTrampoline(std::uintptr_t originalAddr, std::size_t prologueSize)
    {
        auto& trampoline = F4SE::GetTrampoline();
        auto* mem = trampoline.allocate(prologueSize + 14);

        std::memcpy(mem, reinterpret_cast<void*>(originalAddr), prologueSize);

        auto* jmpPtr = static_cast<std::uint8_t*>(mem) + prologueSize;
        jmpPtr[0] = 0xFF; jmpPtr[1] = 0x25;  // jmp [rip+0]
        std::uint32_t zero = 0;
        std::memcpy(&jmpPtr[2], &zero, 4);
        auto target = originalAddr + prologueSize;
        std::memcpy(&jmpPtr[6], &target, 8);

        return mem;
    }

    // Create a trampoline for inflateReset which has a relative JE at offset 3.
    static void* CreateInflateResetTrampoline(std::uintptr_t originalAddr)
    {
        auto& trampoline = F4SE::GetTrampoline();
        auto* mem = static_cast<std::uint8_t*>(trampoline.allocate(40));
        std::size_t off = 0;

        mem[off++] = 0x48; mem[off++] = 0x85; mem[off++] = 0xC9; // test rcx, rcx
        mem[off++] = 0x75; mem[off++] = 0x0E;                     // jne +14

        mem[off++] = 0xFF; mem[off++] = 0x25;                     // jmp [rip+0]
        std::uint32_t zero = 0;
        std::memcpy(&mem[off], &zero, 4); off += 4;
        auto errTarget = originalAddr + 0x1C;
        std::memcpy(&mem[off], &errTarget, 8); off += 8;

        mem[off++] = 0x48; mem[off++] = 0x8B; mem[off++] = 0x41; mem[off++] = 0x28; // mov rax,[rcx+0x28]
        mem[off++] = 0x48; mem[off++] = 0x85; mem[off++] = 0xC0; // test rax, rax

        mem[off++] = 0xFF; mem[off++] = 0x25;                     // jmp [rip+0]
        std::memcpy(&mem[off], &zero, 4); off += 4;
        auto contTarget = originalAddr + 12;
        std::memcpy(&mem[off], &contTarget, 8); off += 8;

        return mem;
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
    // Known zlib 1.2.7 function signatures & layout
    // ========================================================================

    static constexpr std::uint8_t kSig_inflate[] = {
        0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x41, 0x54,
    };
    static constexpr std::uint8_t kSig_inflateEnd[] = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC9,
    };
    static constexpr std::uint8_t kSig_inflateInit2[] = {
        0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x48, 0x83, 0xEC, 0x20,
    };
    static constexpr std::uint8_t kSig_inflateReset[] = {
        0x48, 0x85, 0xC9, 0x74, 0x17, 0x48, 0x8B, 0x41, 0x28,
    };

    static constexpr std::ptrdiff_t kOff_inflateEnd   = 0x1960;
    static constexpr std::ptrdiff_t kOff_inflateInit2 = 0x19F0;
    static constexpr std::ptrdiff_t kOff_inflateReset = 0x1BB0;

    static constexpr std::uintptr_t kOG_offInit = 0x17D;
    static constexpr std::uintptr_t kOG_offInfl = 0x1AF;
    static constexpr std::uintptr_t kNG_offInit = 0x1B2;
    static constexpr std::uintptr_t kNG_offInfl = 0x1E4;

    // ========================================================================
    // Install all hooks
    // ========================================================================

    void Install()
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_qpcFreq = freq.QuadPart;
        s_installTime = QpcNow();

        const bool isVR = REL::Module::IsVR();
        const bool isNG = REL::Module::IsNG();
        const char* runtime = isVR ? "VR" : (isNG ? "NG/AE" : "OG");

        logger::info("runtime: {} (version {})", runtime,
            REL::Module::get().version().string());

        // --- libdeflate for TESFile::DecompressCurrentForm ---
        {
            std::uintptr_t funcBase;
            if (isVR) {
                funcBase = REL::Relocation<std::uintptr_t>{ REL::Offset(0x139100) }.address();
            } else {
                funcBase = REL::Relocation<std::uintptr_t>{
                    REL::RelocationID(116758, 2192561) }.address();
            }

            auto offInit = isNG ? kNG_offInit : kOG_offInit;
            auto offInfl = isNG ? kNG_offInfl : kOG_offInfl;

            auto initOpcode = *reinterpret_cast<const std::uint8_t*>(funcBase + offInit);
            auto inflOpcode = *reinterpret_cast<const std::uint8_t*>(funcBase + offInfl);
            if (initOpcode != 0xE8 || inflOpcode != 0xE8) {
                logger::critical("DecompressCurrentForm: expected CALL (E8) at +{:x}={:02X} +{:x}={:02X}",
                    offInit, initOpcode, offInfl, inflOpcode);
                return;
            }

            auto resolveCall = [](std::uintptr_t callAddr) -> std::uintptr_t {
                auto rel = *reinterpret_cast<const std::int32_t*>(callAddr + 1);
                return callAddr + 5 + rel;
            };
            logger::info("DecompressCurrentForm at {:x}: initCall->{:x} inflateCall->{:x}",
                funcBase, resolveCall(funcBase + offInit), resolveCall(funcBase + offInfl));

            DetourCall(funcBase + offInit, reinterpret_cast<void*>(&Hook_FormInflateInit));
            DetourCall(funcBase + offInfl, reinterpret_cast<void*>(&Hook_FormInflate));

            logger::info("libdeflate: hooked DecompressCurrentForm at {:x} (+{:x}/+{:x})",
                funcBase, offInit, offInfl);
        }

        // --- zlib-ng compat for all other inflate calls (streaming) ---
        {
            std::uintptr_t inflateAddr, inflateInit2Addr, inflateEndAddr, inflateResetAddr;

            if (isVR) {
                inflateAddr      = REL::Relocation<std::uintptr_t>{ REL::Offset(0x1bd66a0) }.address();
                inflateInit2Addr = REL::Relocation<std::uintptr_t>{ REL::Offset(0x1bd8090) }.address();
                inflateEndAddr   = REL::Relocation<std::uintptr_t>{ REL::Offset(0x1bd8000) }.address();
                inflateResetAddr = REL::Relocation<std::uintptr_t>{ REL::Offset(0x1bd8250) }.address();
            } else {
                auto textSeg = REL::Module::get().segment(REL::Segment::text);

                bool found = false;
                std::size_t scanPos = 0;
                while (true) {
                    inflateAddr = ScanForBytes(textSeg.address(), textSeg.size(),
                        kSig_inflate, sizeof(kSig_inflate), &scanPos);

                    if (!inflateAddr) break;

                    inflateEndAddr   = inflateAddr + kOff_inflateEnd;
                    inflateInit2Addr = inflateAddr + kOff_inflateInit2;
                    inflateResetAddr = inflateAddr + kOff_inflateReset;

                    if (VerifyBytes(inflateEndAddr, kSig_inflateEnd, sizeof(kSig_inflateEnd)) &&
                        VerifyBytes(inflateInit2Addr, kSig_inflateInit2, sizeof(kSig_inflateInit2)) &&
                        VerifyBytes(inflateResetAddr, kSig_inflateReset, sizeof(kSig_inflateReset)))
                    {
                        found = true;
                        break;
                    }

                    logger::info("inflate: false positive at {:x}, continuing scan...", inflateAddr);
                }

                if (!found) {
                    logger::critical("could not find inflate with valid derived prologues!");
                    return;
                }
            }

            // Create trampolines (needed for wrapper mode passthrough)
            s_origInflate = reinterpret_cast<inflate_fn>(
                CreateTrampoline(inflateAddr, 12));
            s_origInflateEnd = reinterpret_cast<inflateEnd_fn>(
                CreateTrampoline(inflateEndAddr, 12));
            s_origInflateReset = reinterpret_cast<inflateReset_fn>(
                CreateInflateResetTrampoline(inflateResetAddr));

            // Install hooks
            WriteAbsoluteJump(inflateAddr, reinterpret_cast<void*>(&Hook_inflate));
            WriteAbsoluteJump(inflateInit2Addr, reinterpret_cast<void*>(&Hook_inflateInit2));
            WriteAbsoluteJump(inflateEndAddr, reinterpret_cast<void*>(&Hook_inflateEnd));
            WriteAbsoluteJump(inflateResetAddr, reinterpret_cast<void*>(&Hook_inflateReset));

            logger::info("zlib-ng compat: hooked inflate={:x} init2={:x} end={:x} reset={:x}",
                inflateAddr, inflateInit2Addr, inflateEndAddr, inflateResetAddr);
        }

        logger::info("FastDecompress: all hooks installed (libdeflate + zlib-ng compat {} + lz4 inflate)",
            COMPAT_DIRECT ? "direct" : "wrapper");

        // Background thread: log stats every 15s for first 2 min, then every 60s
        std::thread([]() {
            for (int i = 0; i < 8; i++) {
                std::this_thread::sleep_for(std::chrono::seconds(15));
                LogStats();
            }
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(60));
                LogStats();
            }
        }).detach();
    }
}
