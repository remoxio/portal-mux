#include "ssh/SshConfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>

namespace fs = std::filesystem;

namespace rdc {
namespace {

struct Block {
    std::vector<std::string> patterns; // empty = a Match block, which we never apply
    std::vector<std::pair<std::string, std::string>> options;
};

std::string lower(std::string s)
{
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool glob(const char* p, const char* s)
{
    if (!*p) return !*s;
    if (*p == '*') return glob(p + 1, s) || (*s && glob(p, s + 1));
    return *s && (*p == '?' || std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*s))) &&
           glob(p + 1, s + 1);
}

bool applies(const Block& b, const std::string& host)
{
    bool positive = false;
    for (const std::string& p : b.patterns) {
        if (p[0] == '!') {
            if (glob(p.c_str() + 1, host.c_str())) return false;
        } else if (glob(p.c_str(), host.c_str())) {
            positive = true;
        }
    }
    return positive;
}

std::string home()
{
    const char* h = std::getenv("HOME");
    if (!h) h = std::getenv("USERPROFILE");
    return h ? h : "";
}

std::string expandHome(std::string s)
{
    if (s.starts_with("~/") || s == "~") s.replace(0, 1, home());
    for (size_t at; (at = s.find("%d")) != std::string::npos;) s.replace(at, 2, home());
    return s;
}

std::vector<std::string> words(const std::string& line)
{
    std::vector<std::string> out;
    size_t i = line.find_first_not_of(" \t");
    if (i == std::string::npos || line[i] == '#') return out;
    const size_t end = line.find_first_of(" \t=", i);
    out.push_back(lower(line.substr(i, end - i)));
    if (end == std::string::npos) return out;
    i = line.find_first_not_of(" \t", end);
    if (i != std::string::npos && line[i] == '=') i = line.find_first_not_of(" \t", i + 1);
    while (i != std::string::npos && i < line.size()) {
        if (line[i] == '#') break;
        std::string word;
        if (line[i] == '"') {
            const size_t close = line.find('"', i + 1);
            word = line.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
            i = close == std::string::npos ? line.size() : close + 1;
        } else {
            const size_t stop = line.find_first_of(" \t", i);
            word = line.substr(i, stop - i);
            i = stop;
        }
        out.push_back(word);
        if (i != std::string::npos) i = line.find_first_not_of(" \t", i);
    }
    return out;
}

void parse(const fs::path& file, const fs::path& base, std::vector<Block>& blocks, int depth)
{
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> w = words(line);
        if (w.size() < 2) continue;
        if (w[0] == "host") {
            blocks.push_back({{w.begin() + 1, w.end()}, {}});
        } else if (w[0] == "match") {
            blocks.push_back({});
        } else if (w[0] == "include" && depth < 16) {
            for (size_t i = 1; i < w.size(); ++i) {
                fs::path pattern = expandHome(w[i]);
                if (pattern.is_relative()) pattern = base / pattern;
                const std::string name = pattern.filename().string();
                std::vector<fs::path> files;
                if (name.find_first_of("*?") == std::string::npos) {
                    files.push_back(pattern);
                } else { // shortcut: wildcards in the last component only
                    std::error_code ec;
                    for (const auto& entry : fs::directory_iterator(pattern.parent_path(), ec))
                        if (entry.is_regular_file() && glob(name.c_str(), entry.path().filename().string().c_str()))
                            files.push_back(entry.path());
                    std::sort(files.begin(), files.end());
                }
                for (const fs::path& f : files) parse(f, base, blocks, depth + 1);
            }
        } else {
            blocks.back().options.emplace_back(w[0], w[1]);
        }
    }
}

} // namespace

std::vector<Connection> readSshConfig(const std::string& path)
{
    std::vector<Block> blocks = {{{"*"}, {}}};
    parse(path, fs::path(path).parent_path(), blocks, 0);

    std::vector<std::string> aliases;
    for (const Block& b : blocks)
        for (const std::string& p : b.patterns)
            if (p.find_first_of("*?!") == std::string::npos && std::find(aliases.begin(), aliases.end(), p) == aliases.end())
                aliases.push_back(p);

    std::vector<Connection> result;
    for (const std::string& alias : aliases) {
        std::map<std::string, std::string> opt;
        for (const Block& b : blocks) {
            if (!applies(b, alias)) continue;
            for (const auto& [key, value] : b.options) {
                if (key != "identityfile") opt.try_emplace(key, value);
                else if (!opt.count(key) && fs::exists(expandHome(value))) opt[key] = expandHome(value); // OpenSSH skips missing keys
            }
        }

        Connection c;
        c.id = kSshConfigIdPrefix + alias;
        c.name = alias;
        c.protocol = Protocol::SSH;
        c.host = opt.count("hostname") ? opt["hostname"] : alias;
        for (size_t at; (at = c.host.find("%h")) != std::string::npos;) c.host.replace(at, 2, alias);
        c.username = opt["user"];
        const int port = std::atoi(opt["port"].c_str());
        c.port = port == 22 ? 0 : port;
        if (opt.count("identityfile")) c.options["keyFile"] = opt["identityfile"];
        if (opt.count("proxyjump") && lower(opt["proxyjump"]) != "none") c.options["jumpHost"] = opt["proxyjump"];
        if (lower(opt["compression"]) == "yes") c.options["compression"] = "true";
        if (lower(opt["forwardagent"]) == "yes") c.options["forwardAgent"] = "true";
        if (lower(opt["forwardx11"]) == "yes") c.options["forwardX11"] = "true";
        if (std::atoi(opt["serveraliveinterval"].c_str()) > 0) c.options["keepalive"] = opt["serveraliveinterval"];
        result.push_back(std::move(c));
    }
    return result;
}

} // namespace rdc
