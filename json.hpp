#pragma once
#include <string>
#include <vector>
#include <utility>
#include <initializer_list>
#include <cstddef>
namespace GpuSwitch {
// Output-only JSON; no parser or external runtime dependency.
class Json {
    std::string encoded_ = "null";
    bool array_ = false;
    static std::string quote(const std::string& text) {
        static constexpr char hex[]="0123456789abcdef";
        std::string result="\"";
        for (unsigned char c : text) {
            if (c=='"' || c=='\\') { result+='\\';result+=static_cast<char>(c); }
            else if (c<0x20) { result+="\\u00";result+=hex[c>>4];result+=hex[c&15]; }
            else result+=static_cast<char>(c);
        }
        return result+'"';
    }
  public:
    Json() = default;
    Json(std::nullptr_t) {}
    Json(const std::string& text):encoded_(quote(text)) {}
    Json(const char* text):Json(std::string(text)) {}
    Json(bool value):encoded_(value?"true":"false") {}
    Json(unsigned long value):encoded_(std::to_string(value)) {}
    Json(const std::vector<std::string>& values) {
        *this=array();for (const auto& value:values) push_back(Json(value));
    }
    Json(std::initializer_list<std::pair<std::string,Json>> fields) {
        encoded_="{";
        bool first=true;
        for (const auto& [key,value]:fields) {
            if (!first) encoded_+=',';
            first=false;encoded_+=quote(key)+':'+value.dump();
        }
        encoded_+='}';
    }
    static Json array() { Json result;result.array_=true;result.encoded_="[]";return result; }
    void push_back(const Json& value) {
        if (!array_) return;
        encoded_.pop_back();if (encoded_.size()>1) encoded_+=',';
        encoded_+=value.dump();encoded_+=']';
    }
    std::string dump(int = 0) const { return encoded_; }
};
}
