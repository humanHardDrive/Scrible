#pragma once

// Reading and writing Xournal++ files.
//
// Supported: .xopp and .xoj (gzip-compressed XML) and uncompressed XML.
// Not yet: the zip-packaged .xopp variant (Xournal++ reads it, but doesn't write it).
//
// Strokes are parsed into editable form. Everything else (text, images, LaTeX,
// PDF/image backgrounds, unknown attributes) is kept verbatim and written back.

#include <string>
#include <vector>

#include "Document.h"

namespace scrible {

bool loadXopp(const std::string& path, Document& doc, std::string& error,
              std::vector<std::string>* warnings = nullptr);
bool parseXopp(const std::string& xml, Document& doc, std::string& error,
               std::vector<std::string>* warnings = nullptr);

// Writes gzip-compressed XML via a temp file + rename, so a crash or power loss
// mid-save never leaves a truncated notebook behind.
bool saveXopp(const std::string& path, const Document& doc, std::string& error);
std::string serializeXopp(const Document& doc);

}  // namespace scrible
