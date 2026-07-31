#property strict
#property description "MME feed-only bridge. Contains no order or trade operations."

#include "mme_protocol.mqh"
#include "mme_pipe.mqh"

input string InpPipeName = "\\\\.\\pipe\\mme_md";
input ulong  InpExpectedAccount = 0;
input uint   InpExpectedMarginMode = MME_MARGIN_HEDGING;
input int    InpReconnectSeconds = 5;

// Broker-native symbols for the Exness validation account. The suffix belongs
// to the MT5 bridge only: canonical instrument identity travels the wire as the
// MdRecord.symbol_id index below, so this array's ORDER is the frozen contract.
// 0=EURUSD 1=GBPUSD 2=USDJPY 3=XAUUSD 4=OIL_WTI
string Symbols[MME_SYMBOL_COUNT] = {"EURUSDm", "GBPUSDm", "USDJPYm", "XAUUSDm", "USOILm"};
MmePipe Pipe;
ulong SessionEpoch = 0;
ulong Sequence = 0;
ulong LastTickMsc[MME_SYMBOL_COUNT];
uchar ServerHash[32];
uchar BookSources[MME_SYMBOL_COUNT];

// Transport liveness. A dropped pipe is a recoverable transport condition, not
// a reason to abandon the feed: Part 21 requires a broker/terminal disconnect to
// reconcile on reconnect, and the 14-day recording gate cannot survive a policy
// that removes the EA on the first blip. Identity disagreement is different and
// still refuses outright (Part 5.3 clause 3).
bool     Connected = false;
datetime NextReconnectAt = 0;
ulong    ReconnectAttempts = 0;
ulong    ReconnectCount = 0;

bool HashText(const string text, uchar &digest[]) {
   uchar bytes[], key[];
   int n = StringToCharArray(text, bytes, 0, WHOLE_ARRAY, CP_UTF8);
   if(n <= 0) return false;
   ArrayResize(bytes, n - 1);
   return CryptEncode(CRYPT_HASH_SHA256, bytes, key, digest) == 32;
}

bool EqualHash(const uchar &a[], const uchar &b[]) {
   for(int i = 0; i < 32; ++i) if(a[i] != b[i]) return false;
   return true;
}

void FillPrefix(MmeRecordPrefix &prefix, const ushort type) {
   prefix.magic = MME_PROTOCOL_MAGIC;
   prefix.version = MME_PROTOCOL_VERSION;
   prefix.type = type;
   prefix.session_epoch = SessionEpoch;
   prefix.sequence = ++Sequence;
}

long PriceTicks(const string symbol, const double price) {
   const double tick_size = SymbolInfoDouble(symbol, SYMBOL_TRADE_TICK_SIZE);
   if(tick_size <= 0.0) return 0;
   return (long)MathRound(price / tick_size);
}

void Disconnect(const string reason) {
   if(!Connected) return;
   Connected = false;
   Pipe.Close();
   NextReconnectAt = TimeLocal() + InpReconnectSeconds;
   Print("MME_FEED_DISCONNECT reason=", reason, " retry_in_s=", InpReconnectSeconds,
         " reconnects_so_far=", ReconnectCount);
}

bool SendTick(const int symbol_id, const MqlTick &tick) {
   MmeMdRecord record = {};
   FillPrefix(record.prefix, MME_MARKET_DATA);
   record.market_data_type = MME_MD_QUOTE;
   record.symbol_id = (uint)symbol_id;
   record.ts_broker_ms = tick.time_msc;
   record.ts_terminal_ms = (ulong)TimeTradeServer() * 1000;
   record.bid_ticks = PriceTicks(Symbols[symbol_id], tick.bid);
   record.ask_ticks = PriceTicks(Symbols[symbol_id], tick.ask);
   record.last_ticks = PriceTicks(Symbols[symbol_id], tick.last);
   record.bid_volume = (long)MathRound(tick.volume_real);
   record.ask_volume = record.bid_volume;
   record.tick_volume = (long)tick.volume;
   record.flags = tick.flags;
   if(!Pipe.WriteMarketData(record)) { Disconnect("md_write_failed"); return false; }
   return true;
}

void DrainTicks() {
   if(!Connected) return;
   for(int symbol_id = 0; symbol_id < MME_SYMBOL_COUNT; ++symbol_id) {
      MqlTick ticks[256];
      ulong from = LastTickMsc[symbol_id] == 0 ? 0 : LastTickMsc[symbol_id] + 1;
      int count = CopyTicks(Symbols[symbol_id], ticks, COPY_TICKS_ALL, from, 256);
      for(int i = 0; i < count; ++i) {
         // The watermark only advances on a confirmed send, so ticks buffered
         // during an outage are replayed from MT5 history after reconnect
         // instead of being silently skipped.
         if(!SendTick(symbol_id, ticks[i])) return;
         LastTickMsc[symbol_id] = ticks[i].time_msc;
      }
   }
}

void SendHeartbeat() {
   MmeHbRecord record = {};
   FillPrefix(record.prefix, MME_HEARTBEAT);
   record.ts_terminal_ms = (ulong)TimeTradeServer() * 1000;
   record.account = (ulong)AccountInfoInteger(ACCOUNT_LOGIN);
   for(int i = 0; i < 32; ++i) record.server_hash[i] = ServerHash[i];
   record.trade_mode = (uint)AccountInfoInteger(ACCOUNT_TRADE_MODE);
   record.margin_mode = (uint)AccountInfoInteger(ACCOUNT_MARGIN_MODE);
   record.connected = (uchar)TerminalInfoInteger(TERMINAL_CONNECTED);
   record.trade_allowed = 0; // Safety Rule #0: feed bridge never authorizes trading.
   for(int i = 0; i < MME_SYMBOL_COUNT; ++i) record.book_source[i] = BookSources[i];
   record.equity_minor = (long)MathRound(AccountInfoDouble(ACCOUNT_EQUITY) * 100.0);
   record.balance_minor = (long)MathRound(AccountInfoDouble(ACCOUNT_BALANCE) * 100.0);
   record.margin_minor = (long)MathRound(AccountInfoDouble(ACCOUNT_MARGIN) * 100.0);
   record.free_margin_minor = (long)MathRound(AccountInfoDouble(ACCOUNT_MARGIN_FREE) * 100.0);
   record.engine_hb_age_ms = 0;
   if(!Pipe.WriteHeartbeat(record)) Disconnect("hb_write_failed");
}

// Handshake, shared by first attach and every reconnect.
//   1  connected
//   0  transport unavailable  -> retry, the recorder may simply be restarting
//  -1  identity disagreement  -> refuse to operate (Part 5.3 clause 3)
int TryConnect() {
   if(!Pipe.Open(InpPipeName)) return 0;
   MmeHelloRecord hello = {};
   if(!Pipe.ReadHello(hello)) { Pipe.Close(); return 0; }
   if(hello.prefix.magic != MME_PROTOCOL_MAGIC ||
      hello.prefix.version != MME_PROTOCOL_VERSION) { Pipe.Close(); return -1; }
   if(hello.account_expected != InpExpectedAccount) { Pipe.Close(); return -1; }
   uchar symbol_hash[];
   if(!HashText("EURUSDm,GBPUSDm,USDJPYm,XAUUSDm,USOILm", symbol_hash) ||
      !EqualHash(hello.symbol_set_hash, symbol_hash)) { Pipe.Close(); return -1; }

   // Adopt the recorder's epoch. The supervisor issues a fresh monotonic epoch
   // per start, so records minted before the drop can never be replayed into a
   // new session as live data.
   SessionEpoch = hello.prefix.session_epoch;

   MmeHelloAckRecord ack = {};
   FillPrefix(ack.prefix, MME_HELLO_ACK);
   ack.account_actual = (ulong)AccountInfoInteger(ACCOUNT_LOGIN);
   ack.account_margin_mode = (uint)AccountInfoInteger(ACCOUNT_MARGIN_MODE);
   for(int i = 0; i < 32; ++i) ack.server_hash[i] = ServerHash[i];
   for(int i = 0; i < MME_SYMBOL_COUNT; ++i) ack.book_source[i] = BookSources[i];
   if(!Pipe.WriteHelloAck(ack)) { Pipe.Close(); return 0; }
   return 1;
}

void MaybeReconnect() {
   if(TimeLocal() < NextReconnectAt) return;
   const int verdict = TryConnect();
   if(verdict > 0) {
      Connected = true;
      ReconnectAttempts = 0;
      ReconnectCount++;
      Print("MME_FEED_RECONNECT_OK session_epoch=", SessionEpoch,
            " reconnect_count=", ReconnectCount);
      return;
   }
   if(verdict < 0) {
      Print("MME_FEED_REFUSE reason=identity_mismatch action=manual_review_required");
      ExpertRemove();
      return;
   }
   ReconnectAttempts++;
   NextReconnectAt = TimeLocal() + InpReconnectSeconds;
   // One line per minute at the default interval: enough to see an outage in the
   // Experts log without drowning a 14-day run.
   if(ReconnectAttempts == 1 || (ReconnectAttempts % 12) == 0)
      Print("MME_FEED_RECONNECT_WAIT attempts=", ReconnectAttempts, " pipe=", InpPipeName);
}

int OnInit() {
   if(sizeof(MmeRecordPrefix) != 24 || sizeof(MmeHelloRecord) != 64 ||
      sizeof(MmeHelloAckRecord) != 108 || sizeof(MmeMdRecord) != 100 ||
      sizeof(MmeHbRecord) != 128 || sizeof(MmeSpikeMessage) != 128) {
      Print("MME_PROTOCOL_LAYOUT_REJECT prefix=", sizeof(MmeRecordPrefix),
            " hello=", sizeof(MmeHelloRecord), " ack=", sizeof(MmeHelloAckRecord),
            " md=", sizeof(MmeMdRecord), " hb=", sizeof(MmeHbRecord),
            " spike=", sizeof(MmeSpikeMessage));
      return INIT_FAILED;
   }
   if(InpExpectedAccount == 0 ||
      (ulong)AccountInfoInteger(ACCOUNT_LOGIN) != InpExpectedAccount ||
      (uint)AccountInfoInteger(ACCOUNT_MARGIN_MODE) != InpExpectedMarginMode)
      return INIT_FAILED;
   if(InpReconnectSeconds <= 0) {
      Print("MME_FEED_INIT_REJECT reason=reconnect_interval_not_configured");
      return INIT_FAILED;
   }
   uchar server_hash[];
   if(!HashText(AccountInfoString(ACCOUNT_SERVER), server_hash)) return INIT_FAILED;
   for(int i = 0; i < 32; ++i) ServerHash[i] = server_hash[i];
   for(int i = 0; i < MME_SYMBOL_COUNT; ++i) {
      SymbolSelect(Symbols[i], true);
      BookSources[i] = MME_L1_ONLY; // Fail conservative until an actual book event arrives.
      MarketBookAdd(Symbols[i]);
      LastTickMsc[i] = 0;
   }

   const int verdict = TryConnect();
   if(verdict < 0) {
      Print("MME_FEED_INIT_REJECT reason=identity_mismatch");
      return INIT_FAILED;
   }
   Connected = (verdict > 0);
   if(Connected)
      Print("MME_FEED_CONNECTED session_epoch=", SessionEpoch, " pipe=", InpPipeName);
   else {
      // The recorder may not be listening yet, or may be mid-restart. Attaching
      // in a disconnected state and retrying on the timer is what allows the
      // recording run to outlive a supervisor-driven recorder restart.
      NextReconnectAt = TimeLocal() + InpReconnectSeconds;
      Print("MME_FEED_AWAITING_RECORDER pipe=", InpPipeName,
            " retry_in_s=", InpReconnectSeconds);
   }
   EventSetTimer(1);
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) {
   EventKillTimer();
   for(int i = 0; i < MME_SYMBOL_COUNT; ++i) MarketBookRelease(Symbols[i]);
   Pipe.Close();
}

void OnTick() { DrainTicks(); }
void OnBookEvent(const string &symbol) {
   for(int i = 0; i < MME_SYMBOL_COUNT; ++i)
      if(Symbols[i] == symbol) BookSources[i] = MME_DOM_AGGREGATED;
   DrainTicks();
}
void OnTimer() {
   if(!Connected) { MaybeReconnect(); return; }
   DrainTicks();
   if(Connected) SendHeartbeat();
}
