#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#ifndef MME_FEED_EA_PATH
#error MME_FEED_EA_PATH must be defined
#endif

int main() {
    std::ifstream input(MME_FEED_EA_PATH, std::ios::binary);
    const std::string source((std::istreambuf_iterator<char>(input)), {});
    if (source.empty()) return 1;
    for (const char* forbidden : {"OrderSend", "OrderSendAsync", "CTrade", "PositionOpen",
                                  "PositionClose", "trade.Buy", "trade.Sell"}) {
        if (source.find(forbidden) != std::string::npos) return 1;
    }
    if (source.find("record.trade_allowed = 0") == std::string::npos) return 1;

    // Broker-native symbols for the Exness validation account. The array ORDER is
    // the frozen contract: MdRecord.symbol_id carries canonical identity, so index
    // 0..4 must remain EURUSD, GBPUSD, USDJPY, XAUUSD, OIL_WTI respectively.
    if (source.find("{\"EURUSDm\", \"GBPUSDm\", \"USDJPYm\", \"XAUUSDm\", \"USOILm\"}")
        == std::string::npos) return 1;

    // The hashed string must be the same five symbols, comma-joined in the same
    // order. Drift between the array and this literal is what breaks the Hello
    // handshake, so the two are asserted together and never independently.
    if (source.find("HashText(\"EURUSDm,GBPUSDm,USDJPYm,XAUUSDm,USOILm\"")
        == std::string::npos) return 1;

    // Superseded bare symbols must not survive anywhere in the compilation unit.
    for (const char* stale : {"\"XTIUSD\"", "\"EURUSD\"", "\"GBPUSD\"", "\"USDJPY\"",
                              "\"XAUUSD\"", "XTIUSD,"}) {
        if (source.find(stale) != std::string::npos) return 1;
    }

    std::cout << "feed_ea_no_order_code=pass\n";
    std::cout << "feed_ea_symbol_set=pass\n";
    std::cout << "feed_ea_symbol_hash_agrees=pass\n";
    return 0;
}
