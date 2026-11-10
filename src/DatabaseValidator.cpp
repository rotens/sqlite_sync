#include "DatabaseValidator.hpp"
#include <sqlite3.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

void checkSidecars(const std::filesystem::path& path) {
    for (const auto* suffix:{"-wal","-shm","-journal"}) {
        auto sidecar=path;sidecar+=suffix;
        if (std::filesystem::symlink_status(sidecar).type()!=std::filesystem::file_type::not_found)
            throw std::runtime_error("SQLite sidecar exists: "+sidecar.string()+
                ". Stop all database users and checkpoint/close SQLite before syncing; do not delete sidecars manually.");
    }
}

void validateDatabase(const std::filesystem::path& path) {
    checkSidecars(path);
    std::ifstream file(path,std::ios::binary);
    char header[16]{};
    file.read(header,16);
    if (file.gcount()!=16 || std::string(header,16)!=std::string("SQLite format 3\0",16))
        throw std::runtime_error("Invalid SQLite header: "+path.string());
    // Immutable read-only mode prevents validation from creating or changing sidecars.
    auto utf8=path.generic_u8string();
    std::string uri="file:";
    constexpr char hex[]="0123456789ABCDEF";
    for (unsigned char c:utf8) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='/'||c==':'||c=='-'||c=='_'||c=='.') uri+=static_cast<char>(c);
        else { uri+='%';uri+=hex[c>>4];uri+=hex[c&15]; }
    }
    uri+="?immutable=1";
    sqlite3* raw=nullptr;
    int rc=sqlite3_open_v2(uri.c_str(),&raw,SQLITE_OPEN_READONLY|SQLITE_OPEN_URI,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,sqlite3_close);
    if (rc!=SQLITE_OK) throw std::runtime_error("Cannot open SQLite database: "+path.string());
    sqlite3_stmt* stmt=nullptr;
    rc=sqlite3_prepare_v2(db.get(),"PRAGMA integrity_check",-1,&stmt,nullptr);
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(stmt,sqlite3_finalize);
    if (rc!=SQLITE_OK || sqlite3_step(stmt)!=SQLITE_ROW)
        throw std::runtime_error("SQLite integrity check failed: "+path.string());
    const auto* result=sqlite3_column_text(stmt,0);
    if (!result || std::string(reinterpret_cast<const char*>(result))!="ok" || sqlite3_step(stmt)!=SQLITE_DONE)
        throw std::runtime_error("SQLite integrity check failed: "+path.string());
}
