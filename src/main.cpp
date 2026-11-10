#include "SyncManager.hpp"
#include "Json.hpp"
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
std::filesystem::path fromUtf8(const std::string& value) {
    return std::filesystem::path(std::u8string(value.begin(),value.end()));
}
void help() {
    std::cout<<"sqlite_sync Version 1\n"
        "Usage: sqlite_sync <sync|status> --local PATH --remote PATH [options]\n"
        "  --config PATH       Flat JSON config (paths relative to config directory)\n"
        "  --state-dir PATH    Local metadata directory (default: .sync)\n"
        "  --device-name NAME  Name saved in metadata\n"
        "  --help              Show usage\n"
        "Config keys: local_db, remote_db, state_dir, device_name (all strings).\n"
        "Command-line values override config; CLI paths are relative to the working directory.\n"
        "Exit codes: 0 success, 1 error, 2 conflict. Stop the bot before syncing.\n";
}
int run(const std::vector<std::string>& args) {
    if(args.size()==1 || (args.size()==2&&args[1]=="--help")) {help();return 0;}
    const auto& command=args[1];
    if(command!="sync"&&command!="status") throw std::runtime_error("Unknown command: "+command);
    std::map<std::string,std::string> flags;
    for(std::size_t i=2;i<args.size();++i) {
        auto flag=args[i];
        if(flag=="--help") {help();return 0;}
        if(flag!="--local"&&flag!="--remote"&&flag!="--state-dir"&&flag!="--device-name"&&flag!="--config")
            throw std::runtime_error("Unknown option: "+flag);
        if(++i==args.size() || args[i].starts_with("--")) throw std::runtime_error("Missing value for "+flag);
        if(!flags.emplace(flag,args[i]).second) throw std::runtime_error("Duplicate option: "+flag);
    }
    SyncOptions o;
    if(flags.contains("--config")) {
        auto configPath=std::filesystem::absolute(fromUtf8(flags.at("--config")));
        auto config=readJson(configPath);
        for(const auto& [key,value]:config) {
            if(key!="local_db"&&key!="remote_db"&&key!="state_dir"&&key!="device_name")
                throw std::runtime_error("Unknown config key: "+key);
            if(value.empty()) throw std::runtime_error("Empty config value: "+key);
        }
        auto path=[&](const std::string& key) {
            auto p=fromUtf8(config.at(key));
            return p.is_absolute()?p:configPath.parent_path()/p;
        };
        if(config.contains("local_db")) o.local=path("local_db");
        if(config.contains("remote_db")) o.remote=path("remote_db");
        o.stateDir=config.contains("state_dir")?path("state_dir"):configPath.parent_path()/".sync";
        if(config.contains("device_name")) o.deviceName=config.at("device_name");
    }
    if(flags.contains("--local")) o.local=fromUtf8(flags.at("--local"));
    if(flags.contains("--remote")) o.remote=fromUtf8(flags.at("--remote"));
    if(flags.contains("--state-dir")) o.stateDir=fromUtf8(flags.at("--state-dir"));
    if(flags.contains("--device-name")) o.deviceName=flags.at("--device-name");
    o=normalizeOptions(o);
    // Inspect first so conflicts have a distinct exit code and actionable details.
    auto snapshot=inspect(o);
    if(command=="status" || snapshot.status==SyncStatus::Diverged || snapshot.status==SyncStatus::InitialConflict)
        std::cout<<describe(o,snapshot);
    if(snapshot.status==SyncStatus::Diverged || snapshot.status==SyncStatus::InitialConflict) return 2;
    if(command=="sync") std::cout<<synchronize(o)<<'\n';
    return 0;
}
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
        if(size<=0) {std::cerr<<"Error: invalid Unicode command line\n";return 1;}
        std::string arg(static_cast<std::size_t>(size),'\0');
        WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,arg.data(),size,nullptr,nullptr);
        arg.pop_back();args.push_back(std::move(arg));
    }
#else
int main(int argc,char** argv) {
    std::vector<std::string> args(argv,argv+argc);
#endif
    try {return run(args);}
    catch(const std::exception& error) {std::cerr<<"Error: "<<error.what()<<'\n';return 1;}
}
