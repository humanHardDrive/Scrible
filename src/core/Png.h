#pragma once

#include <string>

#include "Canvas.h"

namespace xs {

// Writes the canvas as an 8-bit grayscale PNG (debugging and the xopp-render tool).
bool writePng(const std::string& path, const Canvas& c, std::string& error);

}  // namespace xs
