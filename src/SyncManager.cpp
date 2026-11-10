#include "SyncManager.hpp"
#include "DatabaseValidator.hpp"
#include "FileHasher.hpp"
#include "Json.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs=std::filesystem;
namespace {
bool present(const fs::path& p) {
    return fs::symlink_status(p).type()!=fs::file_type::not_found;
}
std::string utf8(const fs::path& p) {
    auto s=p.generic_u8string();return {s.begin(),s.end()};
}
fs::path lockPath(fs::path p) {p+=".lock";return p;}
void unlocked(const fs::path& p) {
    if(present(lockPath(p))) throw std::runtime_error("Database appears to be in use: "+p.string()+". Stop the bot before synchronizing.");
}
bool regular(const fs::path& p) {
    if(!present(p)) return false;
    if(fs::symlink_status(p).type()!=fs::file_type::regular)
        throw std::runtime_error("Expected a regular file (no symbolic links): "+p.string());
    if(fs::hard_link_count(p)!=1) throw std::runtime_error("Hard-linked files are not supported: "+p.string());
    return true;
}
std::optional<std::string> databaseHash(const fs::path& p) {
    checkSidecars(p);
    if(!regular(p)) return std::nullopt;
    auto before=hashFile(p);
    validateDatabase(p);
    if(hashFile(p)!=before) throw std::runtime_error("Database changed during validation: "+p.string());
    return before;
}
std::string stamp() {
    auto now=std::chrono::system_clock::now();
    auto time=std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc,&time);
#else
    gmtime_r(&time,&utc);
#endif
    std::ostringstream out;out<<std::put_time(&utc,"%Y-%m-%dT%H%M%SZ");return out.str();
}
class DirectoryGuard {
    fs::path path;
public:
    explicit DirectoryGuard(fs::path p):path(std::move(p)) {
        if(!fs::create_directory(path)) throw std::runtime_error("Lock or staging directory already exists: "+path.string()+". Another sync may be running; inspect before removing a stale lock.");
    }
    DirectoryGuard(const DirectoryGuard&)=delete;
    DirectoryGuard& operator=(const DirectoryGuard&)=delete;
    ~DirectoryGuard() {std::error_code ec;fs::remove_all(path,ec);}
};

void flushFile(const fs::path& p) {
#ifdef _WIN32
    HANDLE handle=CreateFileW(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open staged file for flushing: "+p.string());
    bool ok=FlushFileBuffers(handle)!=0;CloseHandle(handle);
#else
    int fd=::open(p.c_str(),O_WRONLY);
    if(fd<0) throw std::runtime_error("Cannot open staged file for flushing: "+p.string());
    bool ok=::fsync(fd)==0;::close(fd);
#endif
    if(!ok) throw std::runtime_error("Cannot flush staged file: "+p.string());
}

void flushDirectory(const fs::path& p) {
#ifndef _WIN32
    int fd=::open(p.c_str(),O_RDONLY|O_DIRECTORY);
    if(fd<0) throw std::runtime_error("Cannot open directory for flushing: "+p.string());
    bool ok=::fsync(fd)==0;
    ::close(fd);
    if(!ok) throw std::runtime_error("Cannot flush directory: "+p.string());
#else
    (void)p; // Windows replacement uses MOVEFILE_WRITE_THROUGH.
#endif
}

// Stage on the destination filesystem; never fall back to deleting a destination.
void install(const fs::path& staged,const fs::path& destination,bool replace) {
#ifdef _WIN32
    DWORD flags=MOVEFILE_WRITE_THROUGH|(replace?MOVEFILE_REPLACE_EXISTING:0);
    if(!MoveFileExW(staged.c_str(),destination.c_str(),flags))
        throw std::runtime_error("Atomic replacement failed: "+destination.string()+" (Windows error "+std::to_string(GetLastError())+")");
#else
    if(replace) {
        if(::rename(staged.c_str(),destination.c_str())!=0)
            throw std::runtime_error("Atomic replacement failed: "+destination.string()+": "+std::strerror(errno));
    } else {
        // link fails if another process has created the previously missing destination.
        if(::link(staged.c_str(),destination.c_str())!=0)
            throw std::runtime_error("Exclusive installation failed: "+destination.string()+": "+std::strerror(errno));
        if(::unlink(staged.c_str())!=0) throw std::runtime_error("Cannot remove staged hard link: "+staged.string());
    }
    flushDirectory(destination.parent_path());
#endif
}

void expectHash(const fs::path& path,const std::optional<std::string>& expected) {
    checkSidecars(path);
    if(regular(path)!=expected.has_value() || (expected && hashFile(path)!=*expected))
        throw std::runtime_error("File changed during synchronization: "+path.string()+". Aborting; inspect status before retrying.");
}
void recheck(const SyncOptions& o,const SyncSnapshot& s) {
    expectHash(o.local,s.localHash);expectHash(o.remote,s.remoteHash);expectHash(o.stateDir/"base.db",s.baseHash);
}

void verifiedCopy(const fs::path& src,const fs::path& dst,const std::string& hash) {
    fs::copy_file(src,dst,fs::copy_options::none);
    if(hashFile(dst)!=hash) throw std::runtime_error("Copy verification failed: "+dst.string());
    validateDatabase(dst);flushFile(dst);
}

fs::path backup(const SyncOptions& o,const fs::path& source,const std::string& hash,const std::string& label) {
    auto dir=o.stateDir/"backups";fs::create_directories(dir);
    // Exclusive creation, even when several backups share the same timestamp.
    for(unsigned n=0;;++n) {
        auto target=dir/(stamp()+"_"+label+"_"+std::to_string(n)+".db");
        if(present(target)) continue;
        verifiedCopy(source,target,hash);
        flushDirectory(dir);
        return target;
    }
}

SyncSnapshot inspectImpl(const SyncOptions& o,bool checkLocks) {
    if(checkLocks) {
        unlocked(o.local);unlocked(o.remote);
        if(present(o.stateDir/"operation.lock")) throw std::runtime_error("Sync metadata is locked by another operation.");
    }
    SyncSnapshot s;
    // Validate every existing copy, including the base, before deciding an action.
    s.localHash=databaseHash(o.local);s.remoteHash=databaseHash(o.remote);
    s.baseHash=databaseHash(o.stateDir/"base.db");
    bool stateExists=regular(o.stateDir/"state.json");
    if(s.baseHash.has_value()!=stateExists)
        throw std::runtime_error("Incomplete sync metadata: base.db and state.json must both exist. Preserve all copies and recover metadata from a known good backup.");
    if(stateExists) {
        auto state=readJson(o.stateDir/"state.json");
        if(state["format_version"]!="1" || state["local_path"]!=utf8(o.local) ||
           state["remote_path"]!=utf8(o.remote) || state["base_hash"]!=*s.baseHash)
            throw std::runtime_error("Sync metadata paths/version/base hash do not match. Preserve all copies; do not reuse this state directory for another database pair.");
        // Missing files after initialization are ambiguous; never interpret deletion as permission to restore.
        if(!s.localHash||!s.remoteHash)
            throw std::runtime_error("An initialized database is missing. Restore it explicitly from a verified backup before synchronizing.");
    }
    s.status=classify(s);return s;
}

bool samePath(const fs::path& a,const fs::path& b) {
#ifdef _WIN32
    return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;
#else
    return a==b;
#endif
}
bool within(const fs::path& child,const fs::path& parent) {
    auto c=child.begin(),p=parent.begin();
    for(;p!=parent.end();++p,++c) {if(c==child.end()||!samePath(*c,*p)) return false;}
    return true;
}
}

SyncOptions normalizeOptions(SyncOptions o) {
    if(o.local.empty()||o.remote.empty()||o.stateDir.empty()) throw std::runtime_error("Local, remote and state directory paths are required.");
    for(auto* p:{&o.local,&o.remote,&o.stateDir}) {
        auto bytes=utf8(*p);
        if(bytes.find('\0')!=std::string::npos) throw std::runtime_error("NUL is not allowed in paths");
        if(fs::is_symlink(fs::symlink_status(*p))) throw std::runtime_error("Symbolic link paths are not supported: "+p->string());
        *p=fs::weakly_canonical(fs::absolute(*p));
    }
    if(samePath(o.local,o.remote) || (present(o.local)&&present(o.remote)&&fs::equivalent(o.local,o.remote)))
        throw std::runtime_error("Local and remote must be distinct files.");
    if(within(o.local,o.stateDir)||within(o.remote,o.stateDir)||within(o.stateDir,o.local)||within(o.stateDir,o.remote))
        throw std::runtime_error("Database and metadata paths must not overlap.");
    for(const auto& p:{o.local,o.remote}) {
        for(const auto* suffix:{".lock","-wal","-shm","-journal",".sqlite-sync-stage"}) {
            auto reserved=p;reserved+=suffix;
            auto other=p==o.local?o.remote:o.local;
            if(within(other,reserved)||within(o.stateDir,reserved))
                throw std::runtime_error("Paths overlap SQLite sidecars or lock paths.");
        }
    }
    return o;
}

SyncStatus classify(const SyncSnapshot& s) {
    if(!s.baseHash) {
        if(s.localHash&&s.remoteHash&&s.localHash!=s.remoteHash) return SyncStatus::InitialConflict;
        return SyncStatus::Uninitialized;
    }
    if(!s.localHash||!s.remoteHash) throw std::runtime_error("Cannot classify missing initialized database");
    if(s.localHash==s.baseHash && s.remoteHash==s.baseHash) return SyncStatus::Synchronized;
    if(s.localHash==s.baseHash) return SyncStatus::RemoteModified;
    if(s.remoteHash==s.baseHash) return SyncStatus::LocalModified;
    if(s.localHash==s.remoteHash) return SyncStatus::IdenticalIndependentChanges;
    return SyncStatus::Diverged;
}

SyncSnapshot inspect(const SyncOptions& options) {return inspectImpl(normalizeOptions(options),true);}

std::string describe(const SyncOptions& o,const SyncSnapshot& s) {
    std::ostringstream out;
    out<<"Local:   "<<(s.localHash?(s.localHash==s.baseHash?"unchanged":"modified"):"missing")<<"\n"
       <<"Remote:  "<<(s.remoteHash?(s.remoteHash==s.baseHash?"unchanged":"modified"):"missing")<<"\n";
    switch(s.status) {
    case SyncStatus::Synchronized:out<<"Status:  synchronized\n";break;
    case SyncStatus::LocalModified:out<<"Status:  local ahead\nAction:  sync will push local changes\n";break;
    case SyncStatus::RemoteModified:out<<"Status:  remote ahead\nAction:  sync will pull remote changes\n";break;
    case SyncStatus::IdenticalIndependentChanges:out<<"Status:  identical independent changes\nAction:  sync will update the base\n";break;
    case SyncStatus::Diverged:out<<"Status:  DIVERGED - DIVERGENCE DETECTED\nBoth databases changed since last synchronization.\nAutomatic synchronization is disabled. No files were overwritten.\n";break;
    case SyncStatus::InitialConflict:out<<"Initial synchronization conflict.\nLocal and remote databases differ and there is no known common ancestor.\nNo files were overwritten.\n";break;
    case SyncStatus::Uninitialized:out<<"Status:  uninitialized\nAction:  sync will initialize from the existing database(s), if available\n";break;
    }
    out<<"\nLocal:  "<<o.local<<"\n  SHA-256: "<<s.localHash.value_or("missing")
       <<"\nRemote: "<<o.remote<<"\n  SHA-256: "<<s.remoteHash.value_or("missing")
       <<"\nBase:   "<<(o.stateDir/"base.db")<<"\n  SHA-256: "<<s.baseHash.value_or("missing")<<"\n";
    return out.str();
}

std::string synchronize(const SyncOptions& options) {
    auto o=normalizeOptions(options);
    unlocked(o.local);unlocked(o.remote);
    fs::create_directories(o.stateDir);
    DirectoryGuard operation(o.stateDir/"operation.lock");
    // All cooperating tools/bot must honor these same locks. They are local filesystem
    // coordination, not a distributed lock against OneDrive on another computer.
    fs::create_directories(o.local.parent_path());fs::create_directories(o.remote.parent_path());
    DirectoryGuard localLock(lockPath(o.local));
    DirectoryGuard remoteLock(lockPath(o.remote));
    auto s=inspectImpl(o,false);
    if(s.status==SyncStatus::Diverged || s.status==SyncStatus::InitialConflict)
        throw std::runtime_error(describe(o,s));
    if(s.status==SyncStatus::Synchronized) return "Already synchronized. No changes.";
    if(!s.localHash&&!s.remoteHash) throw std::runtime_error("Neither database exists; nothing to initialize.");

    fs::path source;
    std::optional<fs::path> destination;
    std::optional<std::string> previous;
    std::string action,label;
    if(!s.remoteHash || s.status==SyncStatus::LocalModified) {
        source=o.local;destination=o.remote;previous=s.remoteHash;action="PUSH completed.";label="remote";
    } else if(!s.localHash || s.status==SyncStatus::RemoteModified) {
        source=o.remote;destination=o.local;previous=s.localHash;action="PULL completed.";label="local";
    } else {source=o.local;action=s.baseHash?"Identical changes accepted; base updated.":"Synchronization initialized.";}
    auto hash=source==o.local?*s.localHash:*s.remoteHash;
    auto statePath=o.stateDir/"state.json";
    auto oldState=regular(statePath)?std::optional<std::string>(hashFile(statePath)):std::nullopt;

    DirectoryGuard stage(o.stateDir/"operation.lock"/"stage");
    auto baseStage=o.stateDir/"operation.lock"/"stage"/"base.db";
    verifiedCopy(source,baseStage,hash);
    auto stateStage=baseStage.parent_path()/"state.json";
    {
        std::ofstream out(stateStage,std::ios::binary);
        out<<writeJson({{"format_version","1"},{"local_path",utf8(o.local)},{"remote_path",utf8(o.remote)},
                        {"base_hash",hash},{"last_sync_time",stamp()},{"device_name",o.deviceName}});
        out.close();if(!out) throw std::runtime_error("Cannot write staged sync metadata");
        flushFile(stateStage);
    }
    // Preserve the old ancestor as well: a crash between base and manifest commits
    // must leave enough information for explicit recovery rather than guessing.
    if(s.baseHash) {
        auto baseBackup=backup(o,o.stateDir/"base.db",*s.baseHash,"base");
        auto manifestBackup=baseBackup;
        manifestBackup.replace_extension(".state.json");
        fs::copy_file(statePath,manifestBackup,fs::copy_options::none);
        if(hashFile(manifestBackup)!=*oldState) throw std::runtime_error("Metadata backup verification failed");
        flushFile(manifestBackup);
        flushDirectory(manifestBackup.parent_path());
    }
    if(destination) {
        auto stageDir=*destination;stageDir+=".sqlite-sync-stage";
        DirectoryGuard destStage(stageDir);
        auto staged=stageDir/"database.db";
        verifiedCopy(source,staged,hash);
        if(previous) backup(o,*destination,*previous,label);
        recheck(o,s);expectHash(statePath,oldState);
        install(staged,*destination,previous.has_value());
        if(*destination==o.local) s.localHash=hash;else s.remoteHash=hash;
    }
    recheck(o,s);expectHash(statePath,oldState);
    install(baseStage,o.stateDir/"base.db",s.baseHash.has_value());
    // Multi-file commits are not atomic. On interruption, mismatched metadata causes
    // subsequent runs to abort; the old base is retained in backups.
    install(stateStage,statePath,oldState.has_value());
    return action;
}
