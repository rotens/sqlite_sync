#pragma once
#include <filesystem>
#include <optional>
#include <string>

enum class SyncStatus {
    Synchronized, LocalModified, RemoteModified, IdenticalIndependentChanges,
    Diverged, Uninitialized, InitialConflict
};
struct SyncSnapshot {
    std::optional<std::string> localHash,remoteHash,baseHash;
    SyncStatus status=SyncStatus::Uninitialized;
};
struct SyncOptions {
    std::filesystem::path local,remote,stateDir=".sync";
    std::string deviceName="unknown";
};

SyncStatus classify(const SyncSnapshot& snapshot);
SyncOptions normalizeOptions(SyncOptions options);
SyncSnapshot inspect(const SyncOptions& options);
// Returns a description of the completed action. Conflicts/errors throw.
std::string synchronize(const SyncOptions& options);
std::string describe(const SyncOptions& options,const SyncSnapshot& snapshot);
