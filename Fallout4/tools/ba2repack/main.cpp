// ba2repack — repack Fallout 4 BA2 archives with LZ4 compression
//
// Usage: ba2repack.exe <path> [--uncomp] [--level N]
//   <path>    single .ba2 file or directory (processes all .ba2 files)
//   --uncomp  store uncompressed instead of LZ4
//   --level N LZ4 HC compression level (1-12, default 9)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include <lz4.h>
#include <lz4hc.h>
#include <lz4frame.h>
#include <zlib.h>

namespace fs = std::filesystem;

// ============================================================================
// Console / ANSI colors
// ============================================================================

static void EnableAnsiColors()
{
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

// ANSI escape sequences
static constexpr const char* kReset   = "\033[0m";
static constexpr const char* kGreen   = "\033[32m";
static constexpr const char* kDim     = "\033[90m";
static constexpr const char* kBold    = "\033[1m";
static constexpr const char* kCyan    = "\033[36m";
static constexpr const char* kYellow  = "\033[33m";
static constexpr const char* kRed     = "\033[31m";
static constexpr const char* kClearLn = "\033[2K";

// ============================================================================
// Progress bar
// ============================================================================

static constexpr int kBarWidth = 40;

static void PrintProgress(uint32_t current, uint32_t total,
                          uint32_t lz4, uint32_t raw, uint32_t skip)
{
    float pct = (total > 0) ? static_cast<float>(current) / static_cast<float>(total) : 1.0f;
    int filled = static_cast<int>(pct * kBarWidth);

    std::printf("\r%s  %s[%s", kClearLn, kDim, kReset);

    // Green filled portion
    for (int i = 0; i < filled; i++)
        std::printf("%s%s", kGreen, "\xe2\x96\x88");  // UTF-8 full block

    // Dark unfilled portion
    for (int i = filled; i < kBarWidth; i++)
        std::printf("%s%s", kDim, "\xe2\x96\x91");    // UTF-8 light shade

    std::printf("%s%s]%s %3.0f%% %s(%u/%u)%s  ",
        kReset, kDim, kReset,
        pct * 100.0f,
        kDim, current, total, kReset);

    // Mini stats
    std::printf("%slz4:%s%u %sraw:%s%u %sskip:%s%u",
        kGreen, kReset, lz4,
        kCyan, kReset, raw,
        kDim, kReset, skip);

    std::fflush(stdout);
}

static void PrintProgressDone(uint32_t total, uint32_t lz4, uint32_t raw, uint32_t skip,
                              double elapsedSec)
{
    std::printf("\r%s  %s[%s", kClearLn, kDim, kReset);
    for (int i = 0; i < kBarWidth; i++)
        std::printf("%s%s", kGreen, "\xe2\x96\x88");
    std::printf("%s%s]%s %s100%%%s ",
        kReset, kDim, kReset, kBold, kReset);

    std::printf("%s(%u files)%s  ", kDim, total, kReset);
    std::printf("%slz4:%s%u %sraw:%s%u %sskip:%s%u",
        kGreen, kReset, lz4,
        kCyan, kReset, raw,
        kDim, kReset, skip);

    if (elapsedSec >= 0.1)
        std::printf("  %s%.1fs%s", kDim, elapsedSec, kReset);

    std::printf("\n");
    std::fflush(stdout);
}

// ============================================================================
// BA2 structures
// ============================================================================

#pragma pack(push, 1)

struct BA2Header {
    char     magic[4];      // "BTDX"
    uint32_t version;       // 1, 7, or 8
    char     type[4];       // "GNRL" or "DX10"
    uint32_t fileCount;
    uint64_t nameTableOffset;
};
static_assert(sizeof(BA2Header) == 24);

struct GNRLRecord {
    uint32_t nameHash;
    char     ext[4];
    uint32_t dirHash;
    uint32_t flags;
    uint64_t offset;
    uint32_t packedSize;
    uint32_t unpackedSize;
    uint32_t pad;           // 0xBAADF00D
};
static_assert(sizeof(GNRLRecord) == 36);

struct DX10Header {
    uint32_t nameHash;
    char     ext[4];
    uint32_t dirHash;
    uint8_t  unk;
    uint8_t  numChunks;
    uint16_t chunkHdrSize;  // 24
    uint16_t height;
    uint16_t width;
    uint8_t  numMips;
    uint8_t  format;
    uint16_t flags;
};
static_assert(sizeof(DX10Header) == 24);

struct DX10Chunk {
    uint64_t offset;
    uint32_t packedSize;
    uint32_t unpackedSize;
    uint16_t startMip;
    uint16_t endMip;
    uint32_t pad;           // 0xBAADF00D
};
static_assert(sizeof(DX10Chunk) == 24);

#pragma pack(pop)

// LZ4 frame magic: 0x04224D18
static constexpr uint32_t kLZ4FrameMagic = 0x184D2204;

static bool IsLZ4FrameData(const uint8_t* data, uint32_t size) {
    if (size < 4) return false;
    uint32_t magic;
    std::memcpy(&magic, data, 4);
    return magic == kLZ4FrameMagic;
}

// Old custom format for backward compat detection
static constexpr uint8_t kOldLZ4Magic[4] = { 0x04, 'L', 'Z', '4' };
static constexpr uint32_t kOldLZ4HdrSize = 8;

// ============================================================================
// File helpers
// ============================================================================

static FILE* OpenFile(const fs::path& path, const char* mode)
{
    FILE* f = nullptr;
#ifdef _WIN32
    std::wstring wmode;
    for (auto c = mode; *c; ++c)
        wmode.push_back(static_cast<wchar_t>(*c));
    _wfopen_s(&f, path.c_str(), wmode.c_str());
#else
    f = std::fopen(path.string().c_str(), mode);
#endif
    return f;
}

static bool Seek(FILE* f, int64_t offset, int origin)
{
#ifdef _WIN32
    return _fseeki64(f, offset, origin) == 0;
#else
    return fseeko(f, offset, origin) == 0;
#endif
}

static int64_t Tell(FILE* f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}

static bool ReadExact(FILE* f, void* buf, size_t n)
{
    return std::fread(buf, 1, n, f) == n;
}

static bool WriteExact(FILE* f, const void* buf, size_t n)
{
    return std::fwrite(buf, 1, n, f) == n;
}

static std::string FormatSize(uint64_t bytes)
{
    char buf[32];
    if (bytes >= 1024ULL * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ULL * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    return buf;
}

// ============================================================================
// Decompression (zlib)
// ============================================================================

static bool DecompressZlib(const uint8_t* src, uint32_t srcLen,
                           uint8_t* dst, uint32_t dstLen)
{
    uLongf actualLen = dstLen;
    int ret = uncompress(dst, &actualLen, src, srcLen);
    return ret == Z_OK && actualLen == dstLen;
}

// ============================================================================
// Compression (raw LZ4 block with header)
//
// Output format: [4 bytes: "\x04LZ4"] [4 bytes LE: uncompressed_size]
//                [4 bytes LE: lz4_data_size] [N bytes: LZ4 block]
// Total size = 12 + lz4_data_size
// ============================================================================

static constexpr uint8_t kLZ4Magic[4] = { 0x04, 'L', 'Z', '4' };
static constexpr size_t kLZ4HeaderSize = 12;

static size_t CompressLZ4Block(const uint8_t* src, uint32_t srcLen,
                                uint8_t* dst, size_t dstCap, int level)
{
    if (dstCap < kLZ4HeaderSize) return 0;

    int maxLZ4 = static_cast<int>(dstCap - kLZ4HeaderSize);
    int lz4Size = LZ4_compress_HC(
        reinterpret_cast<const char*>(src),
        reinterpret_cast<char*>(dst + kLZ4HeaderSize),
        static_cast<int>(srcLen),
        maxLZ4,
        level);

    if (lz4Size <= 0) return 0;

    // Write header: magic + uncompressed_size + lz4_data_size
    std::memcpy(dst, kLZ4Magic, 4);
    uint32_t uncompSizeLE = srcLen;
    std::memcpy(dst + 4, &uncompSizeLE, 4);
    uint32_t lz4SizeLE = static_cast<uint32_t>(lz4Size);
    std::memcpy(dst + 8, &lz4SizeLE, 4);

    return kLZ4HeaderSize + static_cast<size_t>(lz4Size);
}

static size_t CompressZlib(const uint8_t* src, uint32_t srcLen,
                           uint8_t* dst, size_t dstCap, int level)
{
    uLongf compLen = static_cast<uLongf>(dstCap);
    int ret = compress2(dst, &compLen, src, srcLen, level);
    if (ret != Z_OK) return 0;
    return static_cast<size_t>(compLen);
}

// Compression mode
enum class CompressMode { LZ4, Zlib, Uncomp };

// ============================================================================
// Process a single data block: read, decompress if needed, recompress with LZ4
// Returns new packedSize (0 = stored uncompressed)
// ============================================================================

static bool ProcessBlock(
    FILE* fin, uint64_t offset, uint32_t packedSize, uint32_t unpackedSize,
    FILE* fout, uint64_t* outOffset, uint32_t* outPackedSize,
    CompressMode mode, int compLevel)
{
    // Read raw data from input
    uint32_t readSize = (packedSize > 0) ? packedSize : unpackedSize;
    std::vector<uint8_t> inBuf(readSize);
    if (!Seek(fin, static_cast<int64_t>(offset), SEEK_SET)) return false;
    if (!ReadExact(fin, inBuf.data(), readSize)) return false;

    // Get uncompressed data
    std::vector<uint8_t> uncompBuf;
    const uint8_t* uncompData = nullptr;

    if (packedSize > 0) {
        // Detect existing LZ4 block format (our 12-byte header)
        // Validate: 12 + lz4DataSize must equal readSize
        bool is12ByteLZ4 = false;
        if (readSize >= kLZ4HeaderSize && std::memcmp(inBuf.data(), kLZ4Magic, 4) == 0) {
            uint32_t lz4DataSize;
            std::memcpy(&lz4DataSize, inBuf.data() + 8, 4);
            is12ByteLZ4 = (kLZ4HeaderSize + lz4DataSize == readSize);
        }

        if (is12ByteLZ4 && mode == CompressMode::LZ4) {
            // Pass through if target is also LZ4
            *outOffset = static_cast<uint64_t>(Tell(fout));
            if (!WriteExact(fout, inBuf.data(), readSize)) return false;
            *outPackedSize = packedSize;
            return true;
        }

        if (is12ByteLZ4) {
            // 12-byte header: decompress for re-encoding
            uncompBuf.resize(unpackedSize);
            uint32_t lz4DataSize;
            std::memcpy(&lz4DataSize, inBuf.data() + 8, 4);
            int lz4ret = LZ4_decompress_safe(
                reinterpret_cast<const char*>(inBuf.data() + kLZ4HeaderSize),
                reinterpret_cast<char*>(uncompBuf.data()),
                static_cast<int>(lz4DataSize),
                static_cast<int>(unpackedSize));
            if (lz4ret == static_cast<int>(unpackedSize)) {
                uncompData = uncompBuf.data();
            } else {
                std::fprintf(stderr, "\n  LZ4 12-byte block decompress failed (ret=%d)\n", lz4ret);
                return false;
            }
        } else if (IsLZ4FrameData(inBuf.data(), readSize)) {
            // Legacy LZ4F frame format — decompress
            uncompBuf.resize(unpackedSize);
            size_t srcSz = readSize, dstSz = unpackedSize;
            LZ4F_dctx* dctx = nullptr;
            LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
            size_t lz4ret = LZ4F_decompress(dctx, uncompBuf.data(), &dstSz,
                                             inBuf.data(), &srcSz, nullptr);
            LZ4F_freeDecompressionContext(dctx);
            if (LZ4F_isError(lz4ret) || dstSz != unpackedSize) {
                std::fprintf(stderr, "\n  LZ4F decompress failed\n");
                return false;
            }
            uncompData = uncompBuf.data();
        } else if (readSize >= 4 && std::memcmp(inBuf.data(), kOldLZ4Magic, 4) == 0) {
            // Detect old custom LZ4 format (4-byte or 8-byte header) — decompress first
            uncompBuf.resize(unpackedSize);
            uint32_t hdrSize = 4;
            if (readSize >= kOldLZ4HdrSize) {
                uint32_t embeddedSize;
                std::memcpy(&embeddedSize, inBuf.data() + 4, 4);
                if (kOldLZ4HdrSize + embeddedSize == readSize)
                    hdrSize = kOldLZ4HdrSize;
            }
            int lz4ret = LZ4_decompress_safe(
                reinterpret_cast<const char*>(inBuf.data() + hdrSize),
                reinterpret_cast<char*>(uncompBuf.data()),
                static_cast<int>(readSize - hdrSize),
                static_cast<int>(unpackedSize));
            if (lz4ret == static_cast<int>(unpackedSize)) {
                uncompData = uncompBuf.data();
            } else {
                std::fprintf(stderr, "\n  old-format LZ4 decompress failed (ret=%d)\n", lz4ret);
                return false;
            }
        } else {
            // Decompress zlib
            uncompBuf.resize(unpackedSize);
            if (!DecompressZlib(inBuf.data(), readSize, uncompBuf.data(), unpackedSize)) {
                std::fprintf(stderr, "\n  zlib decompress failed (packed=%u, unpacked=%u)\n",
                    packedSize, unpackedSize);
                return false;
            }
            uncompData = uncompBuf.data();
        }
    } else {
        // Already uncompressed
        uncompData = inBuf.data();
    }

    *outOffset = static_cast<uint64_t>(Tell(fout));

    if (mode == CompressMode::Uncomp) {
        if (!WriteExact(fout, uncompData, unpackedSize)) return false;
        *outPackedSize = 0;
        return true;
    }

    // Compress
    size_t maxOutSize = (mode == CompressMode::LZ4)
        ? static_cast<size_t>(LZ4_compressBound(static_cast<int>(unpackedSize))) + kLZ4HeaderSize
        : compressBound(unpackedSize);
    std::vector<uint8_t> compBuf(maxOutSize);

    size_t compSize = 0;
    if (mode == CompressMode::LZ4) {
        compSize = CompressLZ4Block(uncompData, unpackedSize,
                                    compBuf.data(), maxOutSize, compLevel);
    } else {
        compSize = CompressZlib(uncompData, unpackedSize,
                                compBuf.data(), maxOutSize, compLevel);
    }

    if (compSize == 0) {
        std::fprintf(stderr, "\n  compress failed\n");
        return false;
    }

    // The game's streaming inflate provides 64KB input buffers.
    // Our plugin can handle any output size (internal buffering), but needs
    // all compressed input in a single inflate call.
    // Store raw if compressed data exceeds the input buffer limit.
    if (mode == CompressMode::LZ4 && compSize > 65000) {
        if (!WriteExact(fout, uncompData, unpackedSize)) return false;
        *outPackedSize = 0;
        return true;
    }

    // If compressed is >= uncompressed, store raw instead
    if (compSize >= unpackedSize) {
        if (!WriteExact(fout, uncompData, unpackedSize)) return false;
        *outPackedSize = 0;
        return true;
    }

    if (!WriteExact(fout, compBuf.data(), compSize)) return false;
    *outPackedSize = static_cast<uint32_t>(compSize);
    return true;
}

// ============================================================================
// Repack a single BA2 file
// ============================================================================

static bool RepackBA2(const fs::path& ba2Path, CompressMode mode, int compLevel,
                      int fileIdx, int fileTotal)
{
    auto fileName = ba2Path.filename().string();
    std::printf("\n%s[%d/%d]%s %s%s%s\n",
        kDim, fileIdx, fileTotal, kReset,
        kBold, fileName.c_str(), kReset);

    FILE* fin = OpenFile(ba2Path, "rb");
    if (!fin) {
        std::printf("  %sFailed to open input file%s\n", kRed, kReset);
        return false;
    }

    // Get file size for display
    Seek(fin, 0, SEEK_END);
    uint64_t fileSize = static_cast<uint64_t>(Tell(fin));
    Seek(fin, 0, SEEK_SET);

    // Read header
    BA2Header hdr{};
    if (!ReadExact(fin, &hdr, sizeof(hdr))) {
        std::printf("  %sFailed to read header%s\n", kRed, kReset);
        std::fclose(fin);
        return false;
    }

    if (std::memcmp(hdr.magic, "BTDX", 4) != 0) {
        std::printf("  %sNot a BA2 file (bad magic)%s\n", kRed, kReset);
        std::fclose(fin);
        return false;
    }
    if (hdr.version != 1 && hdr.version != 7 && hdr.version != 8) {
        std::printf("  %sUnsupported BA2 version: %u%s\n", kRed, hdr.version, kReset);
        std::fclose(fin);
        return false;
    }

    bool isGNRL = std::memcmp(hdr.type, "GNRL", 4) == 0;
    bool isDX10 = std::memcmp(hdr.type, "DX10", 4) == 0;
    if (!isGNRL && !isDX10) {
        std::printf("  %sUnknown archive type: %.4s%s\n", kRed, hdr.type, kReset);
        std::fclose(fin);
        return false;
    }

    std::printf("  %s%.4s%s  %sv%u%s  %s%u files%s  %s%s%s\n",
        kCyan, hdr.type, kReset,
        kDim, hdr.version, kReset,
        kDim, hdr.fileCount, kReset,
        kDim, FormatSize(fileSize).c_str(), kReset);

    // Read records
    std::vector<GNRLRecord> gnrlRecords;
    std::vector<DX10Header> dx10Headers;
    std::vector<std::vector<DX10Chunk>> dx10Chunks;

    // Count total blocks for progress
    uint32_t totalBlocks = 0;

    if (isGNRL) {
        gnrlRecords.resize(hdr.fileCount);
        if (!ReadExact(fin, gnrlRecords.data(), hdr.fileCount * sizeof(GNRLRecord))) {
            std::printf("  %sFailed to read GNRL records%s\n", kRed, kReset);
            std::fclose(fin);
            return false;
        }
        totalBlocks = hdr.fileCount;
    } else {
        dx10Headers.resize(hdr.fileCount);
        dx10Chunks.resize(hdr.fileCount);
        for (uint32_t i = 0; i < hdr.fileCount; i++) {
            if (!ReadExact(fin, &dx10Headers[i], sizeof(DX10Header))) {
                std::printf("  %sFailed to read DX10 header %u%s\n", kRed, i, kReset);
                std::fclose(fin);
                return false;
            }
            dx10Chunks[i].resize(dx10Headers[i].numChunks);
            if (!ReadExact(fin, dx10Chunks[i].data(),
                           dx10Headers[i].numChunks * sizeof(DX10Chunk))) {
                std::printf("  %sFailed to read DX10 chunks for file %u%s\n", kRed, i, kReset);
                std::fclose(fin);
                return false;
            }
            totalBlocks += dx10Headers[i].numChunks;
        }
    }

    // Read name table
    std::vector<uint8_t> nameTable;
    if (hdr.nameTableOffset > 0) {
        if (!Seek(fin, static_cast<int64_t>(hdr.nameTableOffset), SEEK_SET)) {
            std::printf("  %sFailed to seek to name table%s\n", kRed, kReset);
            std::fclose(fin);
            return false;
        }
        Seek(fin, 0, SEEK_END);
        int64_t fileEnd = Tell(fin);
        int64_t nameTableSize = fileEnd - static_cast<int64_t>(hdr.nameTableOffset);
        if (nameTableSize > 0) {
            nameTable.resize(static_cast<size_t>(nameTableSize));
            Seek(fin, static_cast<int64_t>(hdr.nameTableOffset), SEEK_SET);
            ReadExact(fin, nameTable.data(), nameTable.size());
        }
    }

    // Open temp output
    fs::path tmpPath = ba2Path;
    tmpPath += ".tmp";
    FILE* fout = OpenFile(tmpPath, "wb");
    if (!fout) {
        std::printf("  %sFailed to create temp file%s\n", kRed, kReset);
        std::fclose(fin);
        return false;
    }

    // Write placeholder header + records (will rewrite later)
    if (!WriteExact(fout, &hdr, sizeof(hdr))) {
        std::printf("  %sFailed to write header%s\n", kRed, kReset);
        std::fclose(fin);
        std::fclose(fout);
        fs::remove(tmpPath);
        return false;
    }

    if (isGNRL) {
        WriteExact(fout, gnrlRecords.data(), gnrlRecords.size() * sizeof(GNRLRecord));
    } else {
        for (uint32_t i = 0; i < hdr.fileCount; i++) {
            WriteExact(fout, &dx10Headers[i], sizeof(DX10Header));
            WriteExact(fout, dx10Chunks[i].data(),
                       dx10Chunks[i].size() * sizeof(DX10Chunk));
        }
    }

    // Process data blocks
    uint32_t blocksDone = 0;
    uint32_t lz4Count = 0;
    uint32_t rawCount = 0;
    uint32_t skippedCount = 0;
    bool ok = true;

    auto startTime = std::chrono::steady_clock::now();

    if (isGNRL) {
        for (uint32_t i = 0; i < hdr.fileCount && ok; i++) {
            auto& rec = gnrlRecords[i];
            if (rec.unpackedSize == 0) {
                rec.offset = static_cast<uint64_t>(Tell(fout));
                blocksDone++;
                if (blocksDone % 200 == 0 || blocksDone == totalBlocks)
                    PrintProgress(blocksDone, totalBlocks, lz4Count, rawCount, skippedCount);
                continue;
            }

            uint64_t newOffset = 0;
            uint32_t newPacked = 0;
            ok = ProcessBlock(fin, rec.offset, rec.packedSize, rec.unpackedSize,
                              fout, &newOffset, &newPacked, mode, compLevel);
            if (ok) {
                rec.offset = newOffset;
                uint32_t oldPacked = rec.packedSize;
                rec.packedSize = newPacked;
                blocksDone++;
                if (newPacked > 0 && !(oldPacked > 0 && oldPacked == newPacked))
                    lz4Count++;
                else if (newPacked == 0)
                    rawCount++;
                else
                    skippedCount++;

                if (blocksDone % 200 == 0 || blocksDone == totalBlocks)
                    PrintProgress(blocksDone, totalBlocks, lz4Count, rawCount, skippedCount);
            }
        }
    } else {
        for (uint32_t i = 0; i < hdr.fileCount && ok; i++) {
            for (uint8_t c = 0; c < dx10Headers[i].numChunks && ok; c++) {
                auto& chunk = dx10Chunks[i][c];
                if (chunk.unpackedSize == 0) {
                    chunk.offset = static_cast<uint64_t>(Tell(fout));
                    blocksDone++;
                    if (blocksDone % 100 == 0 || blocksDone == totalBlocks)
                        PrintProgress(blocksDone, totalBlocks, lz4Count, rawCount, skippedCount);
                    continue;
                }

                uint64_t newOffset = 0;
                uint32_t newPacked = 0;
                ok = ProcessBlock(fin, chunk.offset, chunk.packedSize, chunk.unpackedSize,
                                  fout, &newOffset, &newPacked, mode, compLevel);
                if (ok) {
                    chunk.offset = newOffset;
                    uint32_t oldPacked = chunk.packedSize;
                    chunk.packedSize = newPacked;
                    blocksDone++;
                    if (newPacked > 0 && !(oldPacked > 0 && oldPacked == newPacked))
                        lz4Count++;
                    else if (newPacked == 0)
                        rawCount++;
                    else
                        skippedCount++;

                    if (blocksDone % 100 == 0 || blocksDone == totalBlocks)
                        PrintProgress(blocksDone, totalBlocks, lz4Count, rawCount, skippedCount);
                }
            }
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - startTime).count();

    if (!ok) {
        std::printf("\n  %sProcessing failed at block %u%s\n", kRed, blocksDone, kReset);
        std::fclose(fin);
        std::fclose(fout);
        fs::remove(tmpPath);
        return false;
    }

    // Finish progress bar
    PrintProgressDone(totalBlocks, lz4Count, rawCount, skippedCount, elapsed);

    // Write name table
    uint64_t newNameTableOffset = 0;
    if (!nameTable.empty()) {
        newNameTableOffset = static_cast<uint64_t>(Tell(fout));
        WriteExact(fout, nameTable.data(), nameTable.size());
    }

    // Capture output size before seeking back
    uint64_t outSize = static_cast<uint64_t>(Tell(fout));

    // Rewrite header with updated nameTableOffset
    hdr.nameTableOffset = newNameTableOffset;
    Seek(fout, 0, SEEK_SET);
    WriteExact(fout, &hdr, sizeof(hdr));

    // Rewrite records with updated offsets/packedSizes
    if (isGNRL) {
        WriteExact(fout, gnrlRecords.data(), gnrlRecords.size() * sizeof(GNRLRecord));
    } else {
        for (uint32_t i = 0; i < hdr.fileCount; i++) {
            WriteExact(fout, &dx10Headers[i], sizeof(DX10Header));
            WriteExact(fout, dx10Chunks[i].data(),
                       dx10Chunks[i].size() * sizeof(DX10Chunk));
        }
    }

    std::fclose(fin);
    std::fclose(fout);

    // Rename: original → .bak, temp → original
    fs::path bakPath = ba2Path;
    bakPath += ".bak";

    std::error_code ec;
    if (fs::exists(bakPath)) {
        fs::remove(bakPath, ec);
    }
    fs::rename(ba2Path, bakPath, ec);
    if (ec) {
        std::printf("  %sFailed to rename original to .bak: %s%s\n", kRed, ec.message().c_str(), kReset);
        fs::remove(tmpPath);
        return false;
    }
    fs::rename(tmpPath, ba2Path, ec);
    if (ec) {
        std::printf("  %sFailed to rename temp to original: %s%s\n", kRed, ec.message().c_str(), kReset);
        fs::rename(bakPath, ba2Path);
        return false;
    }

    // Size comparison
    double ratio = (fileSize > 0) ? static_cast<double>(outSize) / static_cast<double>(fileSize) : 1.0;
    std::printf("  %s%s%s -> %s%s%s %s(%.0f%%)%s\n",
        kDim, FormatSize(fileSize).c_str(), kReset,
        kBold, FormatSize(outSize).c_str(), kReset,
        (ratio <= 1.0) ? kGreen : kYellow,
        ratio * 100.0,
        kReset);

    return true;
}

// ============================================================================
// Entry point
// ============================================================================

int main(int argc, char* argv[])
{
    EnableAnsiColors();

    std::printf("\n  %s%sba2repack%s %sv1.2.0%s %s— Fallout 4 BA2 LZ4 repacker%s\n\n",
        kBold, kGreen, kReset, kCyan, kReset, kDim, kReset);

    if (argc < 2) {
        std::printf("  Usage: %sba2repack%s <path> [--zlib] [--uncomp] [--level N]\n\n", kBold, kReset);
        std::printf("    %s<path>%s      single .ba2 file or directory\n", kCyan, kReset);
        std::printf("    %s--zlib%s      compress with zlib (restore original format)\n", kCyan, kReset);
        std::printf("    %s--uncomp%s    store uncompressed\n", kCyan, kReset);
        std::printf("    %s--level N%s   compression level (1-12 for LZ4, 1-9 for zlib, default 9)\n\n", kCyan, kReset);
        return 1;
    }

    CompressMode mode = CompressMode::LZ4;
    int compLevel = 9;
    fs::path inputPath;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--uncomp") == 0) {
            mode = CompressMode::Uncomp;
        } else if (std::strcmp(argv[i], "--zlib") == 0) {
            mode = CompressMode::Zlib;
        } else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            compLevel = std::atoi(argv[++i]);
            if (compLevel < 1) compLevel = 1;
            if (compLevel > 12) compLevel = 12;
        } else if (inputPath.empty()) {
            inputPath = argv[i];
        } else {
            std::fprintf(stderr, "%sUnknown argument: %s%s\n", kRed, argv[i], kReset);
            return 1;
        }
    }

    if (inputPath.empty()) {
        std::fprintf(stderr, "%sNo input path specified%s\n", kRed, kReset);
        return 1;
    }

    if (!fs::exists(inputPath)) {
        std::fprintf(stderr, "%sPath does not exist: %s%s\n", kRed, inputPath.string().c_str(), kReset);
        return 1;
    }

    std::vector<fs::path> files;
    if (fs::is_directory(inputPath)) {
        for (const auto& entry : fs::directory_iterator(inputPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".ba2") {
                files.push_back(entry.path());
            }
        }
        if (files.empty()) {
            std::fprintf(stderr, "%sNo .ba2 files found in: %s%s\n", kRed, inputPath.string().c_str(), kReset);
            return 1;
        }
        std::printf("  Found %s%zu%s BA2 files\n", kBold, files.size(), kReset);
    } else {
        files.push_back(inputPath);
    }

    const char* modeName = (mode == CompressMode::LZ4) ? "LZ4 HC" :
                           (mode == CompressMode::Zlib) ? "zlib" : "uncompressed";
    std::printf("  Mode: %s%s%s", kGreen, modeName, kReset);
    if (mode != CompressMode::Uncomp)
        std::printf(" %s(level %d)%s", kDim, compLevel, kReset);
    std::printf("\n");

    auto totalStart = std::chrono::steady_clock::now();

    int success = 0, fail = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (RepackBA2(files[i], mode, compLevel,
                      static_cast<int>(i + 1), static_cast<int>(files.size())))
            success++;
        else
            fail++;
    }

    auto totalEnd = std::chrono::steady_clock::now();
    double totalElapsed = std::chrono::duration<double>(totalEnd - totalStart).count();

    // Summary
    std::printf("\n  %s%s========================================%s\n", kDim, kDim, kReset);
    if (fail == 0) {
        std::printf("  %s%sAll %d archives repacked successfully%s",
            kBold, kGreen, success, kReset);
    } else {
        std::printf("  %s%d succeeded%s, %s%s%d failed%s",
            kGreen, success, kReset,
            kBold, kRed, fail, kReset);
    }

    // Format total time
    int mins = static_cast<int>(totalElapsed) / 60;
    double secs = totalElapsed - mins * 60;
    std::printf("  %s(", kDim);
    if (mins > 0) std::printf("%dm ", mins);
    std::printf("%.1fs)%s\n\n", secs, kReset);

    return fail > 0 ? 1 : 0;
}
