#include "PCH.h"

#include <spdlog/sinks/basic_file_sink.h>

#include "DecompressHooks.h"

namespace
{
    void InitializeLog()
    {
        auto path = logger::log_directory();
        if (!path) return;

        *path /= "FastDecompress.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
        auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));
        log->set_level(spdlog::level::info);
        log->flush_on(spdlog::level::info);

        spdlog::set_default_logger(std::move(log));
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
    }
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* a_f4se)
{
    F4SE::Init(a_f4se);       // must come first — log_directory() needs GetSaveFolderName()
    InitializeLog();

    const char* runtime = REL::Module::IsVR() ? "VR" :
                           REL::Module::IsNG() ? "NG/AE" : "OG";
    logger::info("FastDecompress v{} loaded (runtime={}, game={})",
        "1.2.0", runtime, REL::Module::get().version().string());

    F4SE::AllocTrampoline(1024);

    FastDecompress::Install();

    return true;
}

extern "C" DLLEXPORT auto F4SEPlugin_Version = []() noexcept {
    F4SE::PluginVersionData v{};
    v.PluginName("FastDecompress");
    v.PluginVersion({ 1, 2, 0, 0 });
    v.UsesSigScanning(true);
    v.HasNoStructUse(true);
    return v;
}();

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(
    const F4SE::QueryInterface*, F4SE::PluginInfo* pluginInfo)
{
    pluginInfo->name = F4SEPlugin_Version.pluginName;
    pluginInfo->infoVersion = F4SE::PluginInfo::kVersion;
    pluginInfo->version = F4SEPlugin_Version.pluginVersion;
    return true;
}
