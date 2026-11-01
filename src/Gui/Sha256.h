// SHA-256 (FIPS 180-4), for the checksums a published dataset carries (SHA256SUMS, Dataset.h), so anyone can show a copy
// is unaltered with `sha256sum -c` or Get-FileHash.  Small and dependency-free; not used for anything secret.
#pragma once
#include <string>

namespace sha256 {
std::string hex(const std::string& bytes);        // lowercase hex digest of the bytes
std::string hexOfFile(const std::string& path);   // "" when the file can't be read
}
