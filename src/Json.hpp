#pragma once
#include <filesystem>
#include <map>
#include <string>

using StringMap=std::map<std::string,std::string>;
// Configuration and state intentionally use flat JSON objects of strings.
StringMap readJson(const std::filesystem::path& path);
std::string writeJson(const StringMap& values);
