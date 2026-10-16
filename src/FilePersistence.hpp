#pragma once

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>

namespace quaddeck {

// Writes through a sibling temporary file and only replaces the destination
// after the complete document has been serialized successfully. A failed
// writer or disk write therefore leaves an existing session/settings file
// untouched instead of truncating it to a partial document.
bool writeUtf8FileAtomically(
    const std::filesystem::path& path,
    const std::function<bool(std::wostream&)>& writer);

// Reads a bounded UTF-8 settings/session document and rejects malformed input
// before its format parser sees any state.
bool readUtf8File(const std::filesystem::path& path, std::wstring& text);

}  // namespace quaddeck
