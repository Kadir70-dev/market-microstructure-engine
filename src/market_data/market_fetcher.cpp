#include "../../include/market_data/market_fetcher.hpp"

#include <string>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;

static size_t WriteCallback(
    void* contents,
    size_t size,
    size_t nmemb,
    std::string* output
) {
    size_t totalSize =
        size * nmemb;

    output->append(
        (char*)contents,
        totalSize
    );

    return totalSize;
}

double fetchMarketPrice(
    const std::string& symbol,
    const std::string& apiKey
) {

    CURL* curl;
    CURLcode res;
    std::string readBuffer;

    std::string url =
        "https://api.twelvedata.com/price"
        "?symbol=" + symbol +
        "&apikey=" + apiKey;

    curl = curl_easy_init();

    if(curl) {

        curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &readBuffer);

        // Stability: no infinite hangs. Typical TwelveData response is <1s.
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,        5L);

        res = curl_easy_perform(curl);

        if(res != CURLE_OK) {

            spdlog::warn(
                "fetch {} curl failed: {}",
                symbol,
                curl_easy_strerror(res)
            );

            curl_easy_cleanup(curl);
            return -1;
        }

        curl_easy_cleanup(curl);
    }
    else {
        spdlog::warn("fetch {}: curl_easy_init failed", symbol);
        return -1;
    }

    // Was previously info-level and logged every cycle: noise and a key-leak
    // risk if upstream ever echoed query parameters. Demoted to debug.
    spdlog::debug(
        "fetch {} raw response: {}",
        symbol,
        readBuffer
    );

    // Wrap parse + stod: a malformed response (rate limit error page,
    // proxy 5xx, empty body, non-numeric "price") would otherwise throw
    // out of this function and kill the engine.
    try {
        auto data = json::parse(readBuffer);

        if(!data.contains("price")) {
            spdlog::warn(
                "fetch {}: response missing 'price' field",
                symbol
            );
            return -1;
        }

        std::string price = data["price"];
        return std::stod(price);
    }
    catch(const std::exception& e) {
        spdlog::warn(
            "fetch {}: parse failure: {}",
            symbol,
            e.what()
        );
        return -1;
    }
}