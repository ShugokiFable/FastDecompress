#pragma once

namespace CrashLogger
{
    // Install vectored exception handler for crash logging.
    // Writes crash details + stack trace to FastDecompressNV_crash.log
    void Install();
}
