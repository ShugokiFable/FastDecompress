#include "PCH.h"

#include <ShlObj.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>

#include "DecompressHooks.h"

namespace
{
    // Read bEnableStats from INI next to the DLL (before log is created)
    bool ReadStatsFromINI()
    {
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ReadStatsFromINI), &hm);
        char dllPath[MAX_PATH]{};
        GetModuleFileNameA(hm, dllPath, MAX_PATH);
        auto iniPath = std::filesystem::path(dllPath).replace_extension(".ini");
        return GetPrivateProfileIntA("General", "bEnableLogging", 0, iniPath.string().c_str()) != 0;
    }

    void InitializeLog()
    {
        // Build the correct log path: My Games/<game>/SKSE/FastDecompressSkyrim.log
        std::filesystem::path logPath;
        wchar_t docsBuf[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docsBuf))) {
            std::filesystem::path docs(docsBuf);
            const wchar_t* gameName = REL::Module::IsVR() ? L"Skyrim VR" : L"Skyrim Special Edition";
            auto skseDir = docs / "My Games" / gameName / "SKSE";
            std::filesystem::create_directories(skseDir);
            logPath = skseDir / "FastDecompressSkyrim.log";
        } else {
            // Fallback: next to DLL
            HMODULE hm = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&InitializeLog), &hm);
            char buf[MAX_PATH]{};
            GetModuleFileNameA(hm, buf, MAX_PATH);
            logPath = std::filesystem::path(buf).parent_path() / "FastDecompressSkyrim.log";
        }
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logPath.string(), true);
        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%H:%M:%S.%e] [%t] [%l] %v");
    }

}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
    SKSE::Init(a_skse);

    bool enableStats = ReadStatsFromINI();

    // Only create log file when stats are enabled
    if (enableStats)
        InitializeLog();

    const char* runtime = REL::Module::IsVR() ? "VR" : "SE/AE";
    logger::info("FastDecompressSkyrim v{} (runtime={}, game={})",
        FastDecompress::kVersion, runtime, REL::Module::get().version().string());

    FastDecompress::Install();

    return true;
}
