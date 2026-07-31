#property strict
#property description "One controlled DEMO trade for mobile verification. Demo-only, min lot, single shot."

// Minimal MT5 demo execution bridge.
//
// Places exactly ONE minimum-lot market order with broker-side SL and TP on the
// demo account, holds it so it is visible in the mobile Trade tab, then closes
// it so it lands in History. It then removes itself.
//
// It is not a strategy: there is no signal, no loop, no re-entry. Structural
// guards make a non-demo account impossible to trade, and the order count is
// capped at one by construction.
//
// Does not touch the engine, recorder, risk logic or PaperBroker.

input long   InpExpectedAccount = 474128546;   // demo account, hard gate
input string InpSymbol          = "EURUSDm";
input int    InpSlPoints        = 200;
input int    InpTpPoints        = 200;
input int    InpHoldSeconds     = 90;          // long enough to see on mobile
input long   InpMagic           = 990222;

ulong    g_ticket   = 0;
datetime g_opened   = 0;
bool     g_done     = false;

int OnInit() {
   // ---- hard gates: demo only, exact account, exact symbol ----------------
   if(AccountInfoInteger(ACCOUNT_TRADE_MODE) != ACCOUNT_TRADE_MODE_DEMO) {
      Print("DEMO_TRADE_REJECT reason=not_demo_account");
      return INIT_FAILED;
   }
   if((long)AccountInfoInteger(ACCOUNT_LOGIN) != InpExpectedAccount) {
      Print("DEMO_TRADE_REJECT reason=account_mismatch actual=",
            AccountInfoInteger(ACCOUNT_LOGIN), " expected=", InpExpectedAccount);
      return INIT_FAILED;
   }
   if(!TerminalInfoInteger(TERMINAL_TRADE_ALLOWED) || !MQLInfoInteger(MQL_TRADE_ALLOWED)) {
      Print("DEMO_TRADE_REJECT reason=algo_trading_disabled");
      return INIT_FAILED;
   }
   if(!SymbolSelect(InpSymbol, true)) {
      Print("DEMO_TRADE_REJECT reason=symbol_unavailable symbol=", InpSymbol);
      return INIT_FAILED;
   }

   // ---- minimum lot, taken from the broker, never assumed ------------------
   const double lots   = SymbolInfoDouble(InpSymbol, SYMBOL_VOLUME_MIN);
   const int    digits = (int)SymbolInfoInteger(InpSymbol, SYMBOL_DIGITS);
   const double point  = SymbolInfoDouble(InpSymbol, SYMBOL_POINT);
   const int    stops  = (int)SymbolInfoInteger(InpSymbol, SYMBOL_TRADE_STOPS_LEVEL);
   if(lots <= 0.0 || point <= 0.0) {
      Print("DEMO_TRADE_REJECT reason=bad_symbol_spec");
      return INIT_FAILED;
   }
   if(InpSlPoints <= stops || InpTpPoints <= stops) {
      Print("DEMO_TRADE_REJECT reason=sl_tp_inside_stops_level stops_level=", stops);
      return INIT_FAILED;
   }

   const double ask = SymbolInfoDouble(InpSymbol, SYMBOL_ASK);
   const double bid = SymbolInfoDouble(InpSymbol, SYMBOL_BID);
   if(ask <= 0.0 || bid <= 0.0) {
      Print("DEMO_TRADE_REJECT reason=no_quote");
      return INIT_FAILED;
   }

   // ---- filling mode resolved from the symbol, not guessed -----------------
   const int flags = (int)SymbolInfoInteger(InpSymbol, SYMBOL_FILLING_MODE);
   ENUM_ORDER_TYPE_FILLING filling = ORDER_FILLING_RETURN;
   if((flags & SYMBOL_FILLING_FOK) != 0)      filling = ORDER_FILLING_FOK;
   else if((flags & SYMBOL_FILLING_IOC) != 0) filling = ORDER_FILLING_IOC;

   MqlTradeRequest req; MqlTradeResult res;
   ZeroMemory(req); ZeroMemory(res);
   req.action       = TRADE_ACTION_DEAL;
   req.symbol       = InpSymbol;
   req.volume       = lots;
   req.type         = ORDER_TYPE_BUY;
   req.price        = ask;
   req.sl           = NormalizeDouble(ask - InpSlPoints * point, digits);
   req.tp           = NormalizeDouble(ask + InpTpPoints * point, digits);
   req.deviation    = 20;
   req.magic        = InpMagic;
   req.type_filling = filling;
   req.comment      = "mme_demo_visibility";

   // Broker-side protection is mandatory (Invariant 1). Never send without both.
   if(req.sl <= 0.0 || req.tp <= 0.0) {
      Print("DEMO_TRADE_REJECT reason=sl_or_tp_unresolved");
      return INIT_FAILED;
   }

   Print("DEMO_TRADE_SEND symbol=", InpSymbol, " lots=", DoubleToString(lots, 2),
         " price=", DoubleToString(ask, digits),
         " sl=", DoubleToString(req.sl, digits), " tp=", DoubleToString(req.tp, digits));

   if(!OrderSend(req, res) ||
      (res.retcode != TRADE_RETCODE_DONE && res.retcode != TRADE_RETCODE_DONE_PARTIAL)) {
      Print("DEMO_TRADE_REJECT retcode=", res.retcode, " comment=", res.comment);
      return INIT_FAILED;
   }

   g_ticket = res.order;
   g_opened = TimeCurrent();
   Print("DEMO_TRADE_OPEN ticket=", g_ticket, " deal=", res.deal,
         " price=", DoubleToString(res.price, digits),
         " lots=", DoubleToString(lots, 2),
         " hold_seconds=", InpHoldSeconds,
         " -> visible in mobile Trade tab now");

   EventSetTimer(1);
   return INIT_SUCCEEDED;
}

void OnDeinit(const int reason) { EventKillTimer(); }

void OnTimer() {
   if(g_done) return;
   if(TimeCurrent() - g_opened < InpHoldSeconds) return;

   // ---- close the single position so it lands in History -------------------
   ulong ticket = 0;
   for(int i = PositionsTotal() - 1; i >= 0; --i) {
      const ulong t = PositionGetTicket(i);
      if(t == 0) continue;
      if(PositionGetString(POSITION_SYMBOL) != InpSymbol) continue;
      if(PositionGetInteger(POSITION_MAGIC) != InpMagic) continue;
      ticket = t;
      break;
   }
   if(ticket == 0) {
      // Already closed by SL/TP: nothing to do, it is in History either way.
      Print("DEMO_TRADE_ALREADY_CLOSED ticket=", g_ticket, " -> check History");
      g_done = true;
      ExpertRemove();
      return;
   }

   if(!PositionSelectByTicket(ticket)) return;
   const double volume = PositionGetDouble(POSITION_VOLUME);
   const double profit = PositionGetDouble(POSITION_PROFIT);

   MqlTradeRequest req; MqlTradeResult res;
   ZeroMemory(req); ZeroMemory(res);
   req.action       = TRADE_ACTION_DEAL;
   req.position     = ticket;
   req.symbol       = InpSymbol;
   req.volume       = volume;
   req.type         = ORDER_TYPE_SELL;   // closing a long
   req.price        = SymbolInfoDouble(InpSymbol, SYMBOL_BID);
   req.deviation    = 20;
   req.magic        = InpMagic;
   req.type_filling = (ENUM_ORDER_TYPE_FILLING)((SymbolInfoInteger(InpSymbol, SYMBOL_FILLING_MODE)
                        & SYMBOL_FILLING_FOK) != 0 ? ORDER_FILLING_FOK : ORDER_FILLING_IOC);
   req.comment      = "mme_demo_close";

   if(!OrderSend(req, res) ||
      (res.retcode != TRADE_RETCODE_DONE && res.retcode != TRADE_RETCODE_DONE_PARTIAL)) {
      Print("DEMO_TRADE_CLOSE_REJECT retcode=", res.retcode, " comment=", res.comment);
      return;   // retried on the next timer tick
   }

   Print("DEMO_TRADE_CLOSED ticket=", ticket, " profit=", DoubleToString(profit, 2),
         " -> now in mobile History");
   g_done = true;
   ExpertRemove();
}

void OnTick() { }
