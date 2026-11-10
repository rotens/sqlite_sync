#include "SyncManager.hpp"
#include "DatabaseValidator.hpp"
#include "FileHasher.hpp"
#include "Json.hpp"
#include <sqlite3.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace fs=std::filesystem;
namespace {
void require(bool value,const std::string& message) {if(!value) throw std::runtime_error(message);}
template<class F> void fails(F action,const std::string& text) {
    try {action();} catch(const std::exception& e) {
        require(std::string(e.what()).find(text)!=std::string::npos,"Unexpected error: "+std::string(e.what()));return;
    }
    throw std::runtime_error("Expected failure: "+text);
}
void write(const fs::path& path,const std::string& bytes) {
    std::ofstream out(path,std::ios::binary);out<<bytes;out.close();require(bool(out),"write failed");
}
void sql(const fs::path& path,const char* statement) {
    sqlite3* db=nullptr;
    auto name=path.u8string();
    int rc=sqlite3_open(reinterpret_cast<const char*>(name.c_str()),&db);
    if(rc!=SQLITE_OK) {if(db) sqlite3_close(db);throw std::runtime_error("test SQLite open failed");}
    rc=sqlite3_exec(db,statement,nullptr,nullptr,nullptr);
    std::string error=sqlite3_errmsg(db);sqlite3_close(db);
    require(rc==SQLITE_OK,"test SQL: "+error);
}
struct Fixture {
    fs::path root;
    SyncOptions o;
    Fixture() {
        auto seed=std::chrono::steady_clock::now().time_since_epoch().count();
        for(unsigned n=0;;++n) {
            root=fs::temp_directory_path()/("sqlite-sync-test-"+std::to_string(seed)+"-"+std::to_string(n));
            if(fs::create_directory(root)) break;
        }
        o={root/"local.db",root/"remote.db",root/".sync","test"};
    }
    ~Fixture() {std::error_code ec;fs::remove_all(root,ec);}
    void local() {sql(o.local,"CREATE TABLE messages(id INTEGER PRIMARY KEY, value TEXT); INSERT INTO messages VALUES(100,'UNREAD');");}
    void init() {local();synchronize(o);}
    void change(const fs::path& p,const char* value) {sql(p,("UPDATE messages SET value='"+std::string(value)+"'").c_str());}
    void equal() {
        auto s=inspect(o);require(s.status==SyncStatus::Synchronized,"not synchronized");
        require(s.localHash==s.remoteHash&&s.localHash==s.baseHash,"hash mismatch");
    }
    bool hasBackup(const std::string& label,const std::string& hash) {
        for(const auto& entry:fs::directory_iterator(o.stateDir/"backups"))
            if(entry.path().filename().string().find("_"+label+"_")!=std::string::npos&&hashFile(entry.path())==hash) return true;
        return false;
    }
};
}

int main() {
    using Test=std::pair<std::string,std::function<void()>>;
    std::vector<Test> tests{
        {"SHA-256 known vectors",[] {
            Fixture f;auto p=f.root/"hash";
            write(p,"");require(hashFile(p)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","empty hash");
            write(p,"abc");require(hashFile(p)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","abc hash");
            write(p,"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
            require(hashFile(p)=="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","padding hash");
            write(p,std::string(1000000,'a'));
            require(hashFile(p)=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","multi-block hash");
        }},
        {"state detection truth table",[] {
            using S=SyncStatus;
            require(classify({"b","b","b"})==S::Synchronized,"clean");
            require(classify({"l","b","b"})==S::LocalModified,"local");
            require(classify({"b","r","b"})==S::RemoteModified,"remote");
            require(classify({"x","x","b"})==S::IdenticalIndependentChanges,"equal changes");
            require(classify({"l","r","b"})==S::Diverged,"divergence");
            require(classify({"l","r",{}})==S::InitialConflict,"initial conflict");
        }},
        {"first-run push",[] {Fixture f;f.local();require(!fs::exists(f.o.remote),"remote present");synchronize(f.o);f.equal();}},
        {"first-run pull",[] {Fixture f;f.local();fs::rename(f.o.local,f.o.remote);synchronize(f.o);f.equal();}},
        {"first-run identical",[] {Fixture f;f.local();fs::copy_file(f.o.local,f.o.remote);synchronize(f.o);f.equal();}},
        {"status is read only",[] {Fixture f;f.local();require(inspect(f.o).status==SyncStatus::Uninitialized,"status");require(!fs::exists(f.o.stateDir),"status wrote metadata");require(!fs::exists(f.o.remote),"status wrote remote");}},
        {"no changes is no-op",[] {Fixture f;f.init();auto state=hashFile(f.o.stateDir/"state.json");synchronize(f.o);f.equal();require(hashFile(f.o.stateDir/"state.json")==state,"metadata rewritten");require(!fs::exists(f.o.stateDir/"backups"),"unnecessary backup");}},
        {"local push and remote backup",[] {Fixture f;f.init();auto old=hashFile(f.o.remote);f.change(f.o.local,"READ");require(inspect(f.o).status==SyncStatus::LocalModified,"local status");synchronize(f.o);f.equal();require(f.hasBackup("remote",old),"remote backup missing");require(f.hasBackup("base",old),"base backup missing");}},
        {"remote pull and local backup",[] {Fixture f;f.init();auto old=hashFile(f.o.local);f.change(f.o.remote,"READ");require(inspect(f.o).status==SyncStatus::RemoteModified,"remote status");synchronize(f.o);f.equal();require(f.hasBackup("local",old),"local backup missing");}},
        {"identical independent changes",[] {Fixture f;f.init();f.change(f.o.local,"READ");fs::copy_file(f.o.local,f.o.remote,fs::copy_options::overwrite_existing);require(inspect(f.o).status==SyncStatus::IdenticalIndependentChanges,"equal changes status");synchronize(f.o);f.equal();}},
        {"divergence preserves every copy",[] {
            Fixture f;f.init();f.change(f.o.local,"READ");f.change(f.o.remote,"READ_KEEP");
            auto l=hashFile(f.o.local),r=hashFile(f.o.remote),b=hashFile(f.o.stateDir/"base.db"),s=hashFile(f.o.stateDir/"state.json");
            require(inspect(f.o).status==SyncStatus::Diverged,"diverged status");
            fails([&]{synchronize(f.o);},"DIVERGENCE DETECTED");
            require(l==hashFile(f.o.local)&&r==hashFile(f.o.remote)&&b==hashFile(f.o.stateDir/"base.db")&&s==hashFile(f.o.stateDir/"state.json"),"conflict changed a file");
        }},
        {"initial conflict preserves copies",[] {Fixture f;f.local();fs::copy_file(f.o.local,f.o.remote);f.change(f.o.remote,"READ");auto l=hashFile(f.o.local),r=hashFile(f.o.remote);fails([&]{synchronize(f.o);},"Initial synchronization conflict");require(l==hashFile(f.o.local)&&r==hashFile(f.o.remote),"initial conflict lost data");require(!fs::exists(f.o.stateDir/"base.db"),"created ancestor without agreement");}},
        {"both databases missing",[] {Fixture f;fails([&]{synchronize(f.o);},"Neither database");require(!fs::exists(f.o.local)&&!fs::exists(f.o.remote),"created empty database");}},
        {"initialized missing remote",[] {Fixture f;f.init();fs::remove(f.o.remote);fails([&]{synchronize(f.o);},"initialized database is missing");require(!fs::exists(f.o.remote),"silently restored remote");}},
        {"initialized missing local",[] {Fixture f;f.init();fs::remove(f.o.local);fails([&]{synchronize(f.o);},"initialized database is missing");}},
        {"missing ancestor with manifest",[] {Fixture f;f.init();fs::remove(f.o.stateDir/"base.db");fails([&]{synchronize(f.o);},"Incomplete sync metadata");}},
        {"missing manifest",[] {Fixture f;f.init();fs::remove(f.o.stateDir/"state.json");fails([&]{synchronize(f.o);},"Incomplete sync metadata");}},
        {"corrupted local",[] {Fixture f;f.init();auto r=hashFile(f.o.remote);write(f.o.local,"corrupt");fails([&]{synchronize(f.o);},"Invalid SQLite header");require(hashFile(f.o.remote)==r,"remote overwritten");}},
        {"corrupted remote",[] {Fixture f;f.init();auto l=hashFile(f.o.local);write(f.o.remote,"corrupt");fails([&]{synchronize(f.o);},"Invalid SQLite header");require(hashFile(f.o.local)==l,"local overwritten");}},
        {"SQLite structural corruption",[] {Fixture f;f.local();fs::resize_file(f.o.local,100);fails([&]{validateDatabase(f.o.local);},"integrity check failed");}},
        {"corrupted base",[] {Fixture f;f.init();write(f.o.stateDir/"base.db","corrupt");fails([&]{synchronize(f.o);},"Invalid SQLite header");}},
        {"local bot lock",[] {Fixture f;f.init();auto lock=f.o.local;lock+=".lock";write(lock,"bot");fails([&]{synchronize(f.o);},"appears to be in use");require(fs::exists(lock),"removed bot lock");}},
        {"remote bot lock",[] {Fixture f;f.init();auto lock=f.o.remote;lock+=".lock";fs::create_directory(lock);fails([&]{inspect(f.o);},"appears to be in use");}},
        {"sync operation lock",[] {Fixture f;f.init();fs::create_directory(f.o.stateDir/"operation.lock");fails([&]{synchronize(f.o);},"already exists");}},
        {"sidecars refused on either side",[] {for(bool remote:{false,true}) for(auto suffix:{"-wal","-shm","-journal"}) {Fixture f;f.init();auto p=remote?f.o.remote:f.o.local;p+=suffix;write(p,"");fails([&]{synchronize(f.o);},"sidecar exists");}}},
        {"orphan sidecar on missing remote",[] {Fixture f;f.local();auto p=f.o.remote;p+="-wal";write(p,"wal");fails([&]{synchronize(f.o);},"sidecar exists");require(!fs::exists(f.o.remote),"overwrote orphan WAL");}},
        {"manifest hash mismatch after interruption",[] {Fixture f;f.init();f.change(f.o.stateDir/"base.db","READ");fails([&]{synchronize(f.o);},"do not match");}},
        {"metadata belongs to different pair",[] {Fixture f;f.init();f.o.remote=f.root/"other.db";fails([&]{inspect(f.o);},"do not match");}},
        {"backup failure preserves destination",[] {Fixture f;f.init();f.change(f.o.local,"READ");auto old=hashFile(f.o.remote);write(f.o.stateDir/"backups","blocked");fails([&]{synchronize(f.o);},"filesystem error");require(old==hashFile(f.o.remote),"overwrote before backup");}},
        {"staging failure preserves destination",[] {Fixture f;f.init();f.change(f.o.local,"READ");auto old=hashFile(f.o.remote);auto p=f.o.remote;p+=".sqlite-sync-stage";fs::create_directory(p);fails([&]{synchronize(f.o);},"already exists");require(old==hashFile(f.o.remote),"overwrote before staging");}},
        {"duplicate paths refused",[] {Fixture f;f.local();f.o.remote=f.o.local;fails([&]{inspect(f.o);},"distinct files");}},
        {"metadata path collision refused",[] {Fixture f;f.o.local=f.o.stateDir/"base.db";fails([&]{inspect(f.o);},"must not overlap");}},
        {"lock path collision refused",[] {Fixture f;f.o.remote=f.o.local;f.o.remote+=".lock";fails([&]{inspect(f.o);},"lock paths");}},
        {"JSON round trip and unicode",[] {Fixture f;auto p=f.root/"config.json";StringMap m{{"path","C:\\data\\a\"b.db"},{"device_name","laptop\n\t"}};write(p,writeJson(m));require(readJson(p)==m,"JSON round trip");write(p,"{\"value\":\"\\u0142\\ud83d\\ude00\"}");require(readJson(p).at("value")=="\xC5\x82\xF0\x9F\x98\x80","unicode JSON");write(p,"{\"a\":\"b\",\"a\":\"c\"}");fails([&]{readJson(p);},"Invalid JSON");}},
        {"paths with URI characters",[] {Fixture f;f.o.local=f.root/"local #?%.db";f.init();f.equal();}}
    };
    unsigned failed=0;
    for(const auto& [name,test]:tests) {
        try {test();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e) {++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    }
    std::cout<<tests.size()-failed<<"/"<<tests.size()<<" tests passed\n";
    return failed?1:0;
}
