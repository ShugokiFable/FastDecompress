#pragma once
// FastDecompressNV: libdeflate + chromium-zlib decompression for Fallout: New Vegas
//
// Two decompression paths:
//   TESFile::DecompressCurrentForm  ->  libdeflate single-shot
//   CompressedArchiveFile::ReadF    ->  libdeflate one-shot (93%) + chromium-zlib SIMD streaming (7%)
//
// ~2.75x faster than stock game, ~2x faster than zlibUpdate

namespace FastDecompress
{
    static constexpr const char* kVersion = "1.2";

    // Install all hooks (call once at plugin load)
    void Install(bool isEditor);
}
