#pragma once

#define NOMMNOSOUND
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
#include <REL/Relocation.h>

#include <Windows.h>

using namespace std::literals;

namespace logger = F4SE::log;

#define DLLEXPORT __declspec(dllexport)

#include <cstdint>
#include <mutex>
#include <unordered_map>
