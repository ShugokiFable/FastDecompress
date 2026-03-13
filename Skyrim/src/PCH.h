#pragma once

#define NOMMNOSOUND
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#include <Windows.h>

using namespace std::literals;

namespace logger = SKSE::log;

#define DLLEXPORT __declspec(dllexport)

#include <cstdint>
#include <mutex>
#include <atomic>
#include <thread>
