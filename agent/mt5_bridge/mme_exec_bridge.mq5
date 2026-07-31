#property strict
#property description "MT5 DEMO execution bridge. File-driven. Demo-only, min lot, one position, mandatory SL/TP."

// Minimal execution bridge: the C++ runtime writes an approved command file,
// this EA validates it against broker truth and places the order.
//
// The C++ side has already passed RiskEngine. These guards are independent and
// deliberately redundant: the bridge must be unable to place a bad order even
// if the writer is wrong.

input long   InpExpectedAccount = 474128546;
input string InpSymbol          = "EURUSDm";
input int    InpSlPoints        = 200;
input int    InpTpPoints        = 200;
input long   InpMagic           = 990333;
input int    InpStaleSeconds    = 60;
// Rejection backoff. A rejected order must not be retried every timer tick:
// the spike EA flooded the broker with 275 NO_MONEY attempts because its rate
// limiter only counted successes. After InpMaxConsecRejects consecutive broker
// rejections the bridge halts itself rather than keep hammering.
input int    InpRejectBackoffSecs = 5;
input int    InpMaxConsecRejects  = 5;
input int    InpTimerMs           = 10;
// Publishes the live top of book for the C++ runtime. The WAL cannot serve
// this: the recorder holds the active segment with dwShareMode=0, so a reader
// only sees a segment once it rotates — measured at exactly 60 s, which is what
// capped execution at one command per minute regardless of anything else.
input bool   InpPublishTick       = true;

string CMD  = "mme_cmd.txt";
string ACK  = "mme_ack.txt";
string KILL = "mme_kill.txt";
string TICK = "mme_tick.txt";

long     g_last_seq = -1;
bool     g_armed    = false;
int      g_consec_rejects = 0;
datetime g_backoff_until  = 0;
double   g_last_bid = 0.0, g_last_ask = 0.0;

int OnInit() {
   if(AccountInfoInteger(ACCOUNT_TRADE_MODE) != ACCOUNT_TRADE_MODE_DEMO) {
      Print("BRIDGE_REJECT reason=not_demo"); return INIT_FAILED; }
   if((long)AccountInfoInteger(ACCOUNT_LOGIN) != InpExpectedAccount) {
      Print("BRIDGE_REJECT reason=account_mismatch actual=", AccountInfoInteger(ACCOUNT_LOGIN));
      return INIT_FAILED; }
   if(!TerminalInfoInteger(TERMINAL_TRADE_ALLOWED) || !MQLInfoInteger(MQL_TRADE_ALLOWED)) {
      Print("BRIDGE_REJECT reason=algo_trading_disabled"); return INIT_FAILED; }
   if(!SymbolSelect(InpSymbol, true)) {
      Print("BRIDGE_REJECT reason=symbol_unavailable"); return INIT_FAILED; }
   g_armed = true;
   // 200 ms was fine when the feed path capped the rate at one command per
   // WAL rotation (60 s). With the live tick path the poll interval becomes a
   // first-order term in the round trip, so it drops to 10 ms.
   EventSetMillisecondTimer(InpTimerMs);
   Print("BRIDGE_READY account=", InpExpectedAccount, " symbol=", InpSymbol,
         " magic=", InpMagic, " cmd_file=", CMD);
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) { EventKillTimer(); }

int OwnPositions() {
   int n = 0;
   for(int i = PositionsTotal() - 1; i >= 0; --i) {
      const ulong t = PositionGetTicket(i);
      if(t == 0) continue;
      if(PositionGetString(POSITION_SYMBOL) != InpSymbol) continue;
      if(PositionGetInteger(POSITION_MAGIC) != InpMagic) continue;
      ++n;
   }
   return n;
}

// Closes every position this EA owns on InpSymbol. Returns the closing ticket,
// or 0 if nothing was closed / the close failed. Closing is a market DEAL in
// the opposite direction with POSITION_BY omitted, which is the netting-safe
// form and works in hedging mode too because the position ticket is explicit.
ulong CloseOwnPositions(const long seq, uint &retcode_out) {
   ulong last_ticket = 0;
   retcode_out = 0;
   for(int i = PositionsTotal() - 1; i >= 0; --i) {
      const ulong t = PositionGetTicket(i);
      if(t == 0) continue;
      if(PositionGetString(POSITION_SYMBOL) != InpSymbol) continue;
      if(PositionGetInteger(POSITION_MAGIC) != InpMagic) continue;

      const long   ptype = PositionGetInteger(POSITION_TYPE);
      const double pvol  = PositionGetDouble(POSITION_VOLUME);
      MqlTick tick;
      if(!SymbolInfoTick(InpSymbol, tick) || tick.bid <= 0.0 || tick.ask <= 0.0) continue;

      const int flags = (int)SymbolInfoInteger(InpSymbol, SYMBOL_FILLING_MODE);
      ENUM_ORDER_TYPE_FILLING fill = ORDER_FILLING_RETURN;
      if((flags & SYMBOL_FILLING_FOK) != 0)      fill = ORDER_FILLING_FOK;
      else if((flags & SYMBOL_FILLING_IOC) != 0) fill = ORDER_FILLING_IOC;

      MqlTradeRequest req; MqlTradeResult res;
      ZeroMemory(req); ZeroMemory(res);
      req.action   = TRADE_ACTION_DEAL;
      req.position = t;                       // close THIS ticket, never net
      req.symbol   = InpSymbol;
      req.volume   = pvol;
      req.type     = (ptype == POSITION_TYPE_BUY) ? ORDER_TYPE_SELL : ORDER_TYPE_BUY;
      req.price    = (ptype == POSITION_TYPE_BUY) ? tick.bid : tick.ask;
      req.deviation = 20;
      req.magic    = InpMagic;
      req.type_filling = fill;
      req.comment  = "mme_bridge_close";
      // No sl/tp on a close: the position is being removed, not protected.

      if(!OrderSend(req, res) ||
         (res.retcode != TRADE_RETCODE_DONE && res.retcode != TRADE_RETCODE_DONE_PARTIAL)) {
         Print("BRIDGE_CLOSE_REJECT seq=", seq, " ticket=", t, " retcode=", res.retcode);
         retcode_out = res.retcode;
         continue;
      }
      Print("BRIDGE_CLOSE_OK seq=", seq, " closed_ticket=", t,
            " close_order=", res.order, " volume=", DoubleToString(pvol,2));
      last_ticket = t;
      retcode_out = res.retcode;
   }
   return last_ticket;
}

void WriteAck(const long seq, const string status, const ulong ticket, const uint retcode,
              const int broker_ms = 0) {
   int h = FileOpen(ACK, FILE_WRITE|FILE_TXT|FILE_ANSI|FILE_COMMON);
   if(h == INVALID_HANDLE) return;
   FileWrite(h, StringFormat("%d,%s,%I64u,%u,%d", seq, status, ticket, retcode, broker_ms));
   FileClose(h);
}

// Live top of book for the C++ runtime: "bid,ask,time_msc,spread_points".
// Written every timer tick and only when the quote actually moved, so a quiet
// market costs nothing.
void PublishTick() {
   if(!InpPublishTick) return;
   MqlTick t;
   if(!SymbolInfoTick(InpSymbol, t) || t.bid <= 0.0 || t.ask <= 0.0) return;
   if(t.bid == g_last_bid && t.ask == g_last_ask) return;
   g_last_bid = t.bid; g_last_ask = t.ask;
   const int dig = (int)SymbolInfoInteger(InpSymbol, SYMBOL_DIGITS);
   const double point = SymbolInfoDouble(InpSymbol, SYMBOL_POINT);
   int h = FileOpen(TICK, FILE_WRITE|FILE_TXT|FILE_ANSI|FILE_COMMON);
   if(h == INVALID_HANDLE) return;
   FileWrite(h, StringFormat("%s,%s,%I64d,%d",
                             DoubleToString(t.bid, dig), DoubleToString(t.ask, dig),
                             (long)t.time_msc,
                             (int)MathRound((t.ask - t.bid) / point)));
   FileClose(h);
}

void OnTimer() {
   if(!g_armed) return;
   PublishTick();

   // Kill switch: a human drops this file into MQL5\Files to stop the bridge.
   if(FileIsExist(KILL, FILE_COMMON)) {
      if(g_last_seq != -999) { Print("BRIDGE_KILLED file=", KILL); g_last_seq = -999; }
      return;
   }
   if(!FileIsExist(CMD, FILE_COMMON)) return;

   int h = FileOpen(CMD, FILE_READ|FILE_TXT|FILE_ANSI|FILE_COMMON);
   if(h == INVALID_HANDLE) return;
   string line = FileReadString(h);
   FileClose(h);

   string parts[];
   if(StringSplit(line, ',', parts) < 3) return;
   const long   seq  = (long)StringToInteger(parts[0]);
   const string side = parts[1];
   const double want = StringToDouble(parts[2]);
   if(seq <= g_last_seq) return;              // already processed; never re-fire

   // Rejection backoff, checked BEFORE the sequence is consumed so a command
   // deferred by backoff is retried rather than silently dropped.
   if(g_backoff_until != 0 && TimeCurrent() < g_backoff_until) return;
   if(InpMaxConsecRejects > 0 && g_consec_rejects >= InpMaxConsecRejects) {
      Print("BRIDGE_HALT reason=consecutive_rejects count=", g_consec_rejects);
      g_last_seq = seq;
      WriteAck(seq, "HALT_CONSEC_REJECTS", 0, 0);
      g_armed = false;                        // requires re-attach; fail closed
      return; }

   g_last_seq = seq;

   // ---- CLOSE / flip leg ---------------------------------------------------
   // The C++ strategy's exit reaches MT5 through this path. Without it the
   // broker position could only ever be closed by its own SL/TP, so the two
   // position states drifted apart and one open ticket blocked every
   // subsequent entry.
   if(side == "CLOSE") {
      if(OwnPositions() == 0) {
         Print("BRIDGE_CLOSE_NOOP seq=", seq, " reason=no_position");
         WriteAck(seq, "CLOSE_NOOP", 0, 0); return; }
      uint rc = 0;
      const ulong closed = CloseOwnPositions(seq, rc);
      if(closed == 0) {
         ++g_consec_rejects;
         g_backoff_until = TimeCurrent() + InpRejectBackoffSecs;
         WriteAck(seq, "REJECT_CLOSE", 0, rc); return; }
      g_consec_rejects = 0; g_backoff_until = 0;
      WriteAck(seq, "CLOSED", closed, rc); return; }

   // ---- independent guards ------------------------------------------------
   // One position maximum, enforced here regardless of what the writer asked
   // for. Never hedged: an opposite-side entry requires a CLOSE first.
   if(OwnPositions() >= 1) {
      Print("BRIDGE_SKIP seq=", seq, " reason=position_already_open");
      WriteAck(seq, "SKIP_MAX_POSITION", 0, 0); return; }

   const double vmin  = SymbolInfoDouble(InpSymbol, SYMBOL_VOLUME_MIN);
   const double vstep = SymbolInfoDouble(InpSymbol, SYMBOL_VOLUME_STEP);
   const int    dig   = (int)SymbolInfoInteger(InpSymbol, SYMBOL_DIGITS);
   const double point = SymbolInfoDouble(InpSymbol, SYMBOL_POINT);
   const int    stops = (int)SymbolInfoInteger(InpSymbol, SYMBOL_TRADE_STOPS_LEVEL);

   // The command's volume field is a lot-STEP COUNT in the C++ model (1 = one
   // minimum lot), not a broker lot size. Comparing it directly against
   // SYMBOL_VOLUME_MIN rejected every command (1 > 0.01). Policy here is
   // minimum lot only, so the requested size is normalised to the broker's
   // step and clamped to the broker minimum.
   if(want <= 0.0) {
      Print("BRIDGE_REJECT seq=", seq, " reason=non_positive_volume want=", want);
      WriteAck(seq, "REJECT_LOT", 0, 0); return; }
   double lots = vmin;
   if(vstep > 0.0) lots = MathRound(lots / vstep) * vstep;
   lots = NormalizeDouble(lots, 2);
   if(lots < vmin) lots = vmin;
   Print("BRIDGE_LOT seq=", seq, " requested=", DoubleToString(want, 2),
         " volume_min=", DoubleToString(vmin, 2), " volume_step=", DoubleToString(vstep, 2),
         " normalised=", DoubleToString(lots, 2));
   if(InpSlPoints <= stops || InpTpPoints <= stops) {
      WriteAck(seq, "REJECT_STOPS", 0, 0); return; }

   MqlTick tick;
   if(!SymbolInfoTick(InpSymbol, tick) || tick.ask <= 0.0 || tick.bid <= 0.0) {
      WriteAck(seq, "REJECT_NO_QUOTE", 0, 0); return; }
   if(TimeCurrent() - tick.time > InpStaleSeconds) {
      Print("BRIDGE_REJECT seq=", seq, " reason=stale_quote");
      WriteAck(seq, "REJECT_STALE", 0, 0); return; }

   const int flags = (int)SymbolInfoInteger(InpSymbol, SYMBOL_FILLING_MODE);
   ENUM_ORDER_TYPE_FILLING fill = ORDER_FILLING_RETURN;
   if((flags & SYMBOL_FILLING_FOK) != 0)      fill = ORDER_FILLING_FOK;
   else if((flags & SYMBOL_FILLING_IOC) != 0) fill = ORDER_FILLING_IOC;

   const bool buy = (side == "BUY");
   MqlTradeRequest req; MqlTradeResult res;
   ZeroMemory(req); ZeroMemory(res);
   req.action = TRADE_ACTION_DEAL;
   req.symbol = InpSymbol;
   req.volume = lots;
   req.type   = buy ? ORDER_TYPE_BUY : ORDER_TYPE_SELL;
   req.price  = buy ? tick.ask : tick.bid;
   req.sl     = NormalizeDouble(buy ? req.price - InpSlPoints*point : req.price + InpSlPoints*point, dig);
   req.tp     = NormalizeDouble(buy ? req.price + InpTpPoints*point : req.price - InpTpPoints*point, dig);
   req.deviation = 20;
   req.magic  = InpMagic;
   req.type_filling = fill;
   req.comment = "mme_bridge";

   if(req.sl <= 0.0 || req.tp <= 0.0) { WriteAck(seq, "REJECT_NO_PROTECTION", 0, 0); return; }

   const ulong t_send = GetMicrosecondCount();
   const bool  ok_send = OrderSend(req, res);
   const int   broker_ms = (int)((GetMicrosecondCount() - t_send) / 1000);
   if(!ok_send ||
      (res.retcode != TRADE_RETCODE_DONE && res.retcode != TRADE_RETCODE_DONE_PARTIAL)) {
      Print("BRIDGE_ORDER_REJECT seq=", seq, " retcode=", res.retcode,
            " consec=", g_consec_rejects + 1);
      ++g_consec_rejects;
      g_backoff_until = TimeCurrent() + InpRejectBackoffSecs;
      WriteAck(seq, "REJECT_BROKER", 0, res.retcode, broker_ms); return; }

   g_consec_rejects = 0; g_backoff_until = 0;
   Print("BRIDGE_ORDER_OK seq=", seq, " side=", side, " ticket=", res.order,
         " lots=", DoubleToString(lots,2), " price=", DoubleToString(res.price,dig),
         " sl=", DoubleToString(req.sl,dig), " tp=", DoubleToString(req.tp,dig),
         " broker_ms=", broker_ms);
   WriteAck(seq, "FILLED", res.order, res.retcode, broker_ms);
}

void OnTick() { }
