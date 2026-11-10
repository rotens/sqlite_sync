#pragma once
#include <filesystem>

// Reject sidecars: copying only a main file could lose committed WAL changes.
void checkSidecars(const std::filesystem::path& path);
void validateDatabase(const std::filesystem::path& path);
