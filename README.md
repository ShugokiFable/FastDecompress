# FastDecompress

Changes decompression systems in Fallout 4, Skyrim (SE/AE) and Fallout New Vegas to newer and faster libraries (libdeflate / liblzma / zstd via vcpkg), cutting load times when the engine reads compressed BSA/BA2 archives.

> Fork of [1001Bits/FastDecompress](https://github.com/1001Bits/FastDecompress).

## Layout

| Directory | Game | Type |
| --- | --- | --- |
| `Skyrim/` | Skyrim SE/AE | SKSE plugin (CMake presets: `debug`, `release`) |
| `Fallout4/` | Fallout 4 | F4SE plugin (CMake + vcpkg) |
| `FalloutNewVegas/` | Fallout New Vegas | NVSE plugin (CMake presets) |

## Building (Skyrim)

Requirements: Visual Studio 2022 (C++ workload), CMake 3.21+, Ninja, vcpkg (`VCPKG_ROOT` set).

```powershell
cd Skyrim
cmake --preset release
cmake --build build/release
```

## License

See [LICENSE](LICENSE).
