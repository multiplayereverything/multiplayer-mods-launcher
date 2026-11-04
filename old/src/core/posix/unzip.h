#pragma once

#include <string>

namespace fl::sys {

// Linux: unpacks `archive` into the existing folder `destDir`, as Windows' tar.exe does there. A .zip (stored or
// deflated, Zip64 too) is read here with zlib; anything else (.tar.gz, .tar.xz) goes to the system's tar. Entries
// that would land outside `destDir` are refused. Unix permissions kept in the zip (a Linux program's x bit) are kept.
bool unpackArchive(const std::wstring& archive, const std::wstring& destDir, std::wstring& error);

}  // namespace fl::sys
