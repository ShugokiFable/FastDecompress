#pragma once

namespace FastDecompress
{
    // Replaces the game's stock zlib 1.2.7 with:
    //   - libdeflate (~2.4x faster) for whole-buffer form decompression
    //   - zlib-ng compat (~2x faster, SIMD) for streaming inflate
    //
    // Supports Fallout 4 OG, NG, AE, and VR.
    void Install();
}
