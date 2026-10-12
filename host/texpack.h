// HD textures: upscaled replacements for the game's textures, from hd_textures.pak next to
// the game files (made by tools/texpack.py; host/texpack.cpp).
#pragma once
#include <string>

namespace texpack {

// Opens the pack and hooks texture uploads; GL context current. False if it cannot be used
// (missing, unreadable, or the GPU lacks BC7).
bool init(const std::string &path);

}  // namespace texpack
