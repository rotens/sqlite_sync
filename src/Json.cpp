#include "Json.hpp"
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>

namespace {
class Parser {
    std::string_view s;
    std::size_t i=0;
    [[noreturn]] void fail() const { throw std::runtime_error("Invalid JSON string object at byte "+std::to_string(i)); }
    void space() { while(i<s.size() && (s[i]==' '||s[i]=='\r'||s[i]=='\n'||s[i]=='\t')) ++i; }
    bool take(char c) { space();if(i<s.size()&&s[i]==c) {++i;return true;}return false; }
    unsigned hex4() {
        unsigned n=0;
        for(int j=0;j<4;++j) {
            if(i==s.size()) fail();
            char c=s[i++];unsigned v;
            if(c>='0'&&c<='9') v=static_cast<unsigned>(c-'0');
            else if(c>='a'&&c<='f') v=static_cast<unsigned>(c-'a'+10);
            else if(c>='A'&&c<='F') v=static_cast<unsigned>(c-'A'+10);
            else fail();
            n=(n<<4)|v;
        }
        return n;
    }
    std::string string() {
        if(!take('"')) fail();
        std::string out;
        while(i<s.size()) {
            unsigned char c=static_cast<unsigned char>(s[i++]);
            if(c=='"') return out;
            if(c<32) fail();
            if(c!='\\') {out+=static_cast<char>(c);continue;}
            if(i==s.size()) fail();
            switch(s[i++]) {
            case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;
            case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u': {
                unsigned cp=hex4();
                if(cp>=0xd800 && cp<=0xdbff) {
                    if(i+2>s.size()||s[i]!='\\'||s[i+1]!='u') fail();
                    i+=2;unsigned lo=hex4();
                    if(lo<0xdc00||lo>0xdfff) fail();
                    cp=0x10000+((cp-0xd800)<<10)+(lo-0xdc00);
                } else if(cp>=0xdc00&&cp<=0xdfff) fail();
                if(cp<0x80) out+=static_cast<char>(cp);
                else {
                    if(cp>=0x10000) out+=static_cast<char>(0xf0|(cp>>18));
                    if(cp>=0x800) out+=static_cast<char>((cp>=0x10000?0x80:0xe0)|((cp>>12)&0x3f));
                    out+=static_cast<char>((cp>=0x800?0x80:0xc0)|((cp>>6)&0x3f));
                    out+=static_cast<char>(0x80|(cp&0x3f));
                }
                break;
            }
            default:fail();
            }
        }
        fail();
    }
public:
    explicit Parser(std::string_view input):s(input) {}
    StringMap parse() {
        StringMap out;
        if(!take('{')) fail();
        if(!take('}')) {
            do {
                auto key=string();if(!take(':')) fail();auto value=string();
                if(!out.emplace(key,value).second) fail();
            } while(take(','));
            if(!take('}')) fail();
        }
        space();if(i!=s.size()) fail();return out;
    }
};

std::string quote(const std::string& value) {
    std::string out="\"";
    constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\') {out+='\\';out+=static_cast<char>(c);}
        else if(c<32) {out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=static_cast<char>(c);
    }
    return out+'"';
}
}

StringMap readJson(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if(!in) throw std::runtime_error("Cannot read JSON: "+path.string());
    std::string content{std::istreambuf_iterator<char>(in),{}};
    if(in.bad()) throw std::runtime_error("Cannot read JSON: "+path.string());
    return Parser(content).parse();
}

std::string writeJson(const StringMap& values) {
    std::string out="{\n";
    bool first=true;
    for(const auto& [k,v]:values) {
        if(!first) out+=",\n";
        first=false;out+="  "+quote(k)+": "+quote(v);
    }
    return out+"\n}\n";
}
