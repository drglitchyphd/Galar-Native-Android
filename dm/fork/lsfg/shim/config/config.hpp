// FORK: stands in for lsfg-vk's config/config.hpp (its TOML config): src/extract/extract.cpp only reads Config::activeConf.dll, left empty here, so the
// DLL is found through LSFG_DLL_PATH_UNIX (fork_lsfg.cpp sets it to the player's Lossless.dll)
#pragma once
#include <string>
namespace Config { struct Conf { std::string dll; }; inline Conf activeConf; }
