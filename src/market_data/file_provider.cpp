#include "market_data/file_provider.hpp"

#include <ctime>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#include <spdlog/spdlog.h>

namespace market_data {

namespace {

long long fileMtime(const std::string& path) {
    struct stat st{};
    if(stat(path.c_str(), &st) != 0) return -1;
    return static_cast<long long>(st.st_mtime);
}

}  // namespace

FileProvider::FileProvider(std::string path, int stale_seconds)
    : path_(std::move(path)), stale_seconds_(stale_seconds) {}

void FileProvider::reloadIfChanged() {
    long long mtime = fileMtime(path_);

    if(mtime < 0) {            // file missing
        last_fresh_ = false;
        last_mtime_ = -1;
        mids_.clear();
        return;
    }

    long long now = static_cast<long long>(std::time(nullptr));
    last_fresh_ = (now - mtime) <= stale_seconds_;

    if(mtime == last_mtime_) return;   // unchanged — keep the parsed map
    last_mtime_ = mtime;

    std::map<std::string, double> parsed;
    std::ifstream f(path_);
    std::string line;
    while(std::getline(f, line)) {
        // MT5's FILE_ANSI writes CRLF; strip the trailing CR.
        if(!line.empty() && line.back() == '\r') line.pop_back();
        if(line.empty()) continue;

        std::stringstream ss(line);
        std::string sym, sts, sbid, sask, smid;
        std::getline(ss, sym,  ',');
        std::getline(ss, sts,  ',');
        std::getline(ss, sbid, ',');
        std::getline(ss, sask, ',');
        std::getline(ss, smid, ',');
        if(sym.empty()) continue;

        double mid = 0.0;
        try {
            if(!smid.empty()) {
                mid = std::stod(smid);
            } else if(!sbid.empty() && !sask.empty()) {
                mid = (std::stod(sbid) + std::stod(sask)) / 2.0;
            }
        } catch(const std::exception&) {
            continue;  // malformed row — skip
        }
        if(mid > 0.0) parsed[sym] = mid;
    }
    mids_.swap(parsed);
}

Quote FileProvider::fetchQuote(const std::string& symbol) {
    reloadIfChanged();
    if(!last_fresh_) {
        return Quote{-1.0};  // missing or stale file → engine skips the symbol
    }
    auto it = mids_.find(symbol);
    if(it == mids_.end()) return Quote{-1.0};
    return Quote{it->second};
}

bool FileProvider::fresh() {
    reloadIfChanged();
    return last_fresh_;
}

}
