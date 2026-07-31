//+------------------------------------------------------------------+
//| mme_spike_scalper.mq5                                            |
//|                                                                   |
//| THROWAWAY DEMO SPIKE. Not part of the institutional engine.       |
//| Purpose: prove the order path end to end and put a correctly      |
//| protected trade on the MT5 mobile app. It is not an edge.         |
//|                                                                   |
//| Structural guarantees:                                            |
//|   * demo account only, re-verified every tick                     |
//|   * at most ONE open position, ever                               |
//|   * fixed lot, never derived from any prior outcome               |
//|   * broker-side SL and TP mandatory on every position             |
//|   * no martingale, no grid, no averaging down, no flip loop       |
//+------------------------------------------------------------------+
#property strict
#property description "Throwaway MT5 demo spike. Demo-only, one position, fixed lot, SL+TP mandatory."

#include "mme_spike_log.mqh"
#include "mme_spike_guards.mqh"
#include "mme_spike_backoff.mqh"

input group "Identity (hard gates)"
input long   InpExpectedAccount   = 474128546;
input string InpExpectedServer    = "Exness-MT5Trial15";
input string InpAllowedSymbol     = "EURUSDm";
input long   InpMagic             = 990111;

input group "Sizing (fixed, never scaled)"
input double InpLots              = 0.01;
input double InpMaxLotHardCap     = 0.01;

input group "Signal"
input int    InpMomentumTicks     = 20;
input int    InpEntryPoints       = 15;

input group "Exit"
input int    InpSlPoints          = 100;
input int    InpTpPoints          = 100;
input int    InpMaxHoldSeconds    = 120;

input group "Limits"
input int    InpMaxSpreadPoints   = 20;
input int    InpCooldownSeconds   = 30;
input int    InpMaxTradesPerMin   = 3;
input int    InpMaxTradesPerDay   = 200;
input double InpMaxDailyLossPct   = 2.0;
input int    InpMaxConsecLosses   = 5;
input int    InpPauseMinutes      = 15;
input int    InpSessionStartHour  = 0;
input int    InpSessionEndHour    = 24;
input int    InpDeviationPoints   = 20;
input string InpKillFile          = "MME_SPIKE_KILL";

input group "Rejection handling"
input int    InpRejectBackoffSecs = 5;
input int    InpMaxConsecRejects  = 5;

//--- Resolved symbol facts.
SpikeSymbolSpec g_spec;

//--- Rolling mid-price ring for the momentum window.
double g_mids[];
int    g_mid_count = 0;
int    g_mid_head  = 0;

//--- Lifecycle state.
bool     g_enabled          = false;
bool     g_halted           = false;
datetime g_last_close_time  = 0;
datetime g_pause_until      = 0;
int      g_day_of_year      = -1;
double   g_day_start_equity = 0.0;

//--- Broker-rejection backoff, shared by the open and close paths.
SpikeRejectState g_rejects;

//--- Counters.
int      g_trades_today     = 0;
int      g_wins_today       = 0;
int      g_losses_today     = 0;
double   g_gross_today      = 0.0;
int      g_consec_losses    = 0;
datetime g_minute_window    = 0;
int      g_trades_in_minute = 0;

//--- Milestone reporting thresholds.
int      g_next_milestone   = 1;

//+------------------------------------------------------------------+
int OnInit()
{
   g_enabled = false;
   g_halted  = false;

   if(!SpikeGuardAccount(InpExpectedAccount, InpExpectedServer)) return(INIT_FAILED);
   if(!SpikeGuardSymbol(_Symbol, InpAllowedSymbol))              return(INIT_FAILED);
   if(!SpikeGuardLots(_Symbol, InpLots, InpMaxLotHardCap, g_spec))return(INIT_FAILED);
   if(!SpikeGuardFilling(_Symbol, g_spec))                       return(INIT_FAILED);
   if(!SpikeGuardStops(g_spec, InpSlPoints, InpTpPoints))        return(INIT_FAILED);
   if(!SpikeGuardAutoTrading())                                  return(INIT_FAILED);

   if(InpMomentumTicks < 2)
   {
      SpikeLog("INIT_REJECT", "reason=momentum_window_too_small");
      return(INIT_FAILED);
   }
   if(InpRejectBackoffSecs <= 0 || InpMaxConsecRejects <= 0)
   {
      SpikeLog("INIT_REJECT", "reason=reject_backoff_not_configured " +
               SpikeI("backoff_s", InpRejectBackoffSecs) + " " +
               SpikeI("max_consec", InpMaxConsecRejects));
      return(INIT_FAILED);
   }
   if(SpikeKillActive(InpKillFile))
   {
      SpikeLog("INIT_REJECT", "reason=kill_file_present " + SpikeS("file", InpKillFile));
      return(INIT_FAILED);
   }

   SpikeRejectReset(g_rejects);

   ArrayResize(g_mids, InpMomentumTicks);
   ArrayInitialize(g_mids, 0.0);
   g_mid_count = 0;
   g_mid_head  = 0;

   ResetDay();
   g_enabled = true;

   SpikeLog("INIT_OK",
      SpikeI("account", AccountInfoInteger(ACCOUNT_LOGIN)) + " " +
      SpikeS("server", AccountInfoString(ACCOUNT_SERVER)) + " " +
      SpikeS("mode", "DEMO") + " " +
      SpikeS("symbol", _Symbol) + " " +
      SpikeD("lots", InpLots, 2) + " " +
      SpikeI("digits", g_spec.digits) + " " +
      SpikeI("stops_level", g_spec.stops_level) + " " +
      SpikeI("freeze_level", g_spec.freeze_level) + " " +
      SpikeI("filling_mode", g_spec.filling_mode) + " " +
      SpikeI("sl_points", InpSlPoints) + " " +
      SpikeI("tp_points", InpTpPoints) + " " +
      SpikeI("max_trades_day", InpMaxTradesPerDay) + " " +
      SpikeI("reject_backoff_s", InpRejectBackoffSecs) + " " +
      SpikeI("max_consec_rejects", InpMaxConsecRejects) + " " +
      SpikeI("magic", InpMagic));

   EventSetTimer(1);
   return(INIT_SUCCEEDED);
}

//+------------------------------------------------------------------+
void OnDeinit(const int reason)
{
   EventKillTimer();
   SpikeLog("DEINIT", SpikeI("reason", reason) + " " + SpikeI("trades_today", g_trades_today));
}

//+------------------------------------------------------------------+
void ResetDay()
{
   MqlDateTime now;
   TimeToStruct(TimeTradeServer(), now);
   g_day_of_year      = now.day_of_year;
   g_day_start_equity = AccountInfoDouble(ACCOUNT_EQUITY);
   g_trades_today     = 0;
   g_wins_today       = 0;
   g_losses_today     = 0;
   g_gross_today      = 0.0;
   g_consec_losses    = 0;
   g_next_milestone   = 1;
   SpikeLog("DAY_START", SpikeI("day_of_year", g_day_of_year) + " " +
            SpikeD("equity", g_day_start_equity, 2));
}

void MaybeRollDay()
{
   MqlDateTime now;
   TimeToStruct(TimeTradeServer(), now);
   if(now.day_of_year != g_day_of_year)
   {
      LogDailySummary("day_roll");
      ResetDay();
      g_halted = false;
   }
}

void LogDailySummary(const string reason)
{
   SpikeLog("DAILY_SUMMARY",
      SpikeS("reason", reason) + " " +
      SpikeI("trades", g_trades_today) + " " +
      SpikeI("wins", g_wins_today) + " " +
      SpikeI("losses", g_losses_today) + " " +
      SpikeD("net", g_gross_today, 2) + " " +
      SpikeD("equity", AccountInfoDouble(ACCOUNT_EQUITY), 2));
}

//+------------------------------------------------------------------+
//| Position helpers. Only positions carrying our magic are visible  |
//| to this EA; anything else on the account is left strictly alone. |
//+------------------------------------------------------------------+
ulong OwnPositionTicket()
{
   for(int i = PositionsTotal() - 1; i >= 0; i--)
   {
      const ulong ticket = PositionGetTicket(i);
      if(ticket == 0) continue;
      if(PositionGetString(POSITION_SYMBOL) != _Symbol) continue;
      if(PositionGetInteger(POSITION_MAGIC) != InpMagic) continue;
      return(ticket);
   }
   return(0);
}

int OwnPositionCount()
{
   int count = 0;
   for(int i = PositionsTotal() - 1; i >= 0; i--)
   {
      const ulong ticket = PositionGetTicket(i);
      if(ticket == 0) continue;
      if(PositionGetString(POSITION_SYMBOL) != _Symbol) continue;
      if(PositionGetInteger(POSITION_MAGIC) != InpMagic) continue;
      count++;
   }
   return(count);
}

//+------------------------------------------------------------------+
bool ClosePosition(const ulong ticket, const string reason)
{
   if(!PositionSelectByTicket(ticket)) return(false);

   const long   type   = PositionGetInteger(POSITION_TYPE);
   const double volume = PositionGetDouble(POSITION_VOLUME);
   const double profit = PositionGetDouble(POSITION_PROFIT);

   MqlTradeRequest request;
   MqlTradeResult  result;
   ZeroMemory(request);
   ZeroMemory(result);

   request.action       = TRADE_ACTION_DEAL;
   request.position     = ticket;
   request.symbol       = _Symbol;
   request.volume       = volume;
   request.deviation    = InpDeviationPoints;
   request.magic        = InpMagic;
   request.type_filling = (ENUM_ORDER_TYPE_FILLING)g_spec.filling_mode;
   request.comment      = "spike_close";

   if(type == POSITION_TYPE_BUY)
   {
      request.type  = ORDER_TYPE_SELL;
      request.price = SymbolInfoDouble(_Symbol, SYMBOL_BID);
   }
   else
   {
      request.type  = ORDER_TYPE_BUY;
      request.price = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   }

   SpikeLog("ORDER_REQUEST", "kind=close " + SpikeI("ticket", (long)ticket) + " " +
            SpikeS("reason", reason) + " " + SpikeD("volume", volume, 2));

   const bool sent = OrderSend(request, result);
   SpikeLog("ORDER_RESULT", "kind=close " + SpikeI("retcode", result.retcode) + " " +
            SpikeS("retcode_text", SpikeRetcodeText(result.retcode)) + " " +
            SpikeD("price", result.price, g_spec.digits) + " " +
            SpikeD("projected_profit", profit, 2));

   if(!sent || (result.retcode != TRADE_RETCODE_DONE &&
                result.retcode != TRADE_RETCODE_DONE_PARTIAL))
   {
      //--- Backing off a failing close is safe precisely because broker-side
      //--- SL and TP are mandatory: the position stays protected while we wait.
      SpikeRejectOnFailure(g_rejects, TimeTradeServer(), InpRejectBackoffSecs);
      SpikeLog("REJECT", "kind=close " + SpikeI("retcode", result.retcode) + " " +
               SpikeS("retcode_text", SpikeRetcodeText(result.retcode)) + " " +
               SpikeI("consec_rejects", g_rejects.consec) + " " +
               SpikeI("blocked_for_s", (long)(g_rejects.block_until - TimeTradeServer())));
      return(false);
   }

   SpikeRejectOnSuccess(g_rejects);
   return(true);
}

void FlattenAll(const string reason)
{
   for(int attempt = 0; attempt < 5; attempt++)
   {
      const ulong ticket = OwnPositionTicket();
      if(ticket == 0) break;
      if(!ClosePosition(ticket, reason)) break;
   }
}

//+------------------------------------------------------------------+
void Halt(const string reason)
{
   if(g_halted) return;
   g_halted = true;
   FlattenAll(reason);
   LogDailySummary(reason);
   SpikeLog("HALTED", SpikeS("reason", reason) + " action=manual_restart_required");
}

//+------------------------------------------------------------------+
//| Signal: momentum of the mid over a fixed tick window.            |
//| Returns +1 long, -1 short, 0 neutral.                            |
//+------------------------------------------------------------------+
int EvaluateSignal(const double mid)
{
   const int oldest = (g_mid_head + InpMomentumTicks - g_mid_count) % InpMomentumTicks;
   const double reference = g_mids[oldest];

   g_mids[g_mid_head] = mid;
   g_mid_head = (g_mid_head + 1) % InpMomentumTicks;
   if(g_mid_count < InpMomentumTicks) { g_mid_count++; return(0); }

   const double move_points = (mid - reference) / g_spec.point;
   if(move_points >= (double)InpEntryPoints)  return(1);
   if(move_points <= -(double)InpEntryPoints) return(-1);
   return(0);
}

//+------------------------------------------------------------------+
bool RateGatesAllow()
{
   const datetime now = TimeTradeServer();

   //--- A rejected order buys a quiet period. The per-minute counter cannot
   //--- do this job: it only advances on orders the broker actually accepted.
   if(SpikeRejectBlocked(g_rejects, now))
      return(false);

   if(g_trades_today >= InpMaxTradesPerDay)
   {
      SpikeLog("LIMIT_HIT", "limit=max_trades_per_day " + SpikeI("value", g_trades_today));
      return(false);
   }
   if(now < g_pause_until)
      return(false);
   if(now - g_last_close_time < InpCooldownSeconds)
      return(false);

   const datetime minute_bucket = now - (now % 60);
   if(minute_bucket != g_minute_window)
   {
      g_minute_window    = minute_bucket;
      g_trades_in_minute = 0;
   }
   if(g_trades_in_minute >= InpMaxTradesPerMin)
   {
      SpikeLog("LIMIT_HIT", "limit=max_trades_per_minute " + SpikeI("value", g_trades_in_minute));
      return(false);
   }

   MqlDateTime tm;
   TimeToStruct(now, tm);
   if(tm.hour < InpSessionStartHour || tm.hour >= InpSessionEndHour)
      return(false);

   return(true);
}

//+------------------------------------------------------------------+
bool OpenPosition(const int direction)
{
   const double ask = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   const double bid = SymbolInfoDouble(_Symbol, SYMBOL_BID);
   if(ask <= 0.0 || bid <= 0.0) return(false);

   MqlTradeRequest request;
   MqlTradeResult  result;
   ZeroMemory(request);
   ZeroMemory(result);

   request.action       = TRADE_ACTION_DEAL;
   request.symbol       = _Symbol;
   request.volume       = InpLots;
   request.deviation    = InpDeviationPoints;
   request.magic        = InpMagic;
   request.type_filling = (ENUM_ORDER_TYPE_FILLING)g_spec.filling_mode;
   request.comment      = "spike_open";

   const double sl_distance = InpSlPoints * g_spec.point;
   const double tp_distance = InpTpPoints * g_spec.point;

   if(direction > 0)
   {
      request.type  = ORDER_TYPE_BUY;
      request.price = ask;
      request.sl    = NormalizeDouble(ask - sl_distance, g_spec.digits);
      request.tp    = NormalizeDouble(ask + tp_distance, g_spec.digits);
   }
   else
   {
      request.type  = ORDER_TYPE_SELL;
      request.price = bid;
      request.sl    = NormalizeDouble(bid + sl_distance, g_spec.digits);
      request.tp    = NormalizeDouble(bid - tp_distance, g_spec.digits);
   }

   //--- Broker-side protection is mandatory. Never send without both.
   if(request.sl <= 0.0 || request.tp <= 0.0)
   {
      SpikeLog("REJECT", "kind=open reason=sl_or_tp_unresolved");
      return(false);
   }

   SpikeLog("ORDER_REQUEST", "kind=open " +
      SpikeS("side", direction > 0 ? "BUY" : "SELL") + " " +
      SpikeD("lots", request.volume, 2) + " " +
      SpikeD("price", request.price, g_spec.digits) + " " +
      SpikeD("sl", request.sl, g_spec.digits) + " " +
      SpikeD("tp", request.tp, g_spec.digits) + " " +
      SpikeI("deviation", request.deviation) + " " +
      SpikeI("filling", g_spec.filling_mode));

   const bool sent = OrderSend(request, result);

   SpikeLog("ORDER_RESULT", "kind=open " +
      SpikeI("retcode", result.retcode) + " " +
      SpikeS("retcode_text", SpikeRetcodeText(result.retcode)) + " " +
      SpikeI("deal", (long)result.deal) + " " +
      SpikeI("order", (long)result.order) + " " +
      SpikeD("price", result.price, g_spec.digits));

   if(!sent || (result.retcode != TRADE_RETCODE_DONE &&
                result.retcode != TRADE_RETCODE_DONE_PARTIAL))
   {
      //--- Without this the next tick retries immediately and a persistent
      //--- rejection becomes an unbounded order flood at tick rate.
      SpikeRejectOnFailure(g_rejects, TimeTradeServer(), InpRejectBackoffSecs);
      SpikeLog("REJECT", "kind=open " + SpikeI("retcode", result.retcode) + " " +
               SpikeS("retcode_text", SpikeRetcodeText(result.retcode)) + " " +
               SpikeI("consec_rejects", g_rejects.consec) + " " +
               SpikeI("blocked_for_s", (long)(g_rejects.block_until - TimeTradeServer())));
      return(false);
   }

   SpikeRejectOnSuccess(g_rejects);
   g_trades_today++;
   g_trades_in_minute++;
   return(true);
}

//+------------------------------------------------------------------+
//| Post-fill verification: a position without broker-side SL or TP  |
//| is closed immediately. A dead EA must never leave one unguarded. |
//+------------------------------------------------------------------+
void VerifyProtection(const ulong ticket)
{
   if(!PositionSelectByTicket(ticket)) return;
   const double sl = PositionGetDouble(POSITION_SL);
   const double tp = PositionGetDouble(POSITION_TP);
   if(sl > 0.0 && tp > 0.0) return;

   SpikeLog("REJECT", "kind=unprotected_position " + SpikeI("ticket", (long)ticket) + " " +
            SpikeD("sl", sl, g_spec.digits) + " " + SpikeD("tp", tp, g_spec.digits));
   ClosePosition(ticket, "missing_protection");
   Halt("unprotected_position");
}

//+------------------------------------------------------------------+
void ManageOpenPosition(const ulong ticket)
{
   if(!PositionSelectByTicket(ticket)) return;

   const datetime now = TimeTradeServer();
   if(SpikeRejectBlocked(g_rejects, now)) return;

   const datetime opened = (datetime)PositionGetInteger(POSITION_TIME);
   if(now - opened < InpMaxHoldSeconds) return;

   if(!ClosePosition(ticket, "time_stop") &&
      SpikeRejectExhausted(g_rejects, InpMaxConsecRejects))
      Halt("consecutive_rejects");
}

//+------------------------------------------------------------------+
bool DailyLossBreached()
{
   if(g_day_start_equity <= 0.0) return(false);
   const double equity = AccountInfoDouble(ACCOUNT_EQUITY);
   const double drop_pct = (g_day_start_equity - equity) / g_day_start_equity * 100.0;
   return(drop_pct >= InpMaxDailyLossPct);
}

//+------------------------------------------------------------------+
void OnTick()
{
   if(!g_enabled) return;

   //--- Demo re-verification. Anything else halts immediately.
   if(!SpikeStillDemo(InpExpectedAccount))
   {
      Halt("account_or_mode_changed");
      g_enabled = false;
      return;
   }

   if(SpikeKillActive(InpKillFile))
   {
      if(!g_halted)
      {
         SpikeLog("KILL_TRIGGERED", SpikeS("source", "file") + " " +
                  SpikeI("open_positions", OwnPositionCount()));
         Halt("kill_file");
      }
      return;
   }

   MaybeRollDay();
   if(g_halted) return;

   const int open_count = OwnPositionCount();

   //--- Invariant: never more than one position. Anything else is a bug.
   if(open_count > 1)
   {
      SpikeLog("REJECT", "kind=duplicate_position " + SpikeI("count", open_count));
      Halt("duplicate_position");
      return;
   }

   if(DailyLossBreached())
   {
      SpikeLog("LIMIT_HIT", "limit=max_daily_loss " +
               SpikeD("start_equity", g_day_start_equity, 2) + " " +
               SpikeD("equity", AccountInfoDouble(ACCOUNT_EQUITY), 2));
      Halt("max_daily_loss");
      return;
   }

   const double bid = SymbolInfoDouble(_Symbol, SYMBOL_BID);
   const double ask = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   if(bid <= 0.0 || ask <= 0.0) return;
   const double mid = (bid + ask) / 2.0;

   //--- The window is fed on every tick so the signal is continuous.
   const int signal = EvaluateSignal(mid);

   if(open_count == 1)
   {
      const ulong ticket = OwnPositionTicket();
      VerifyProtection(ticket);
      if(!g_halted) ManageOpenPosition(ticket);
      return;
   }

   if(signal == 0) return;

   const double spread_points = (ask - bid) / g_spec.point;
   if(spread_points > (double)InpMaxSpreadPoints)
   {
      SpikeLog("GATE_BLOCK", "gate=spread " + SpikeD("spread", spread_points, 1) + " " +
               SpikeI("max", InpMaxSpreadPoints));
      return;
   }

   if(!RateGatesAllow()) return;

   SpikeLog("SIGNAL",
      SpikeS("direction", signal > 0 ? "LONG" : "SHORT") + " " +
      SpikeD("spread_points", spread_points, 1) + " " +
      SpikeD("bid", bid, g_spec.digits) + " " +
      SpikeD("ask", ask, g_spec.digits));

   if(!OpenPosition(signal) &&
      SpikeRejectExhausted(g_rejects, InpMaxConsecRejects))
      Halt("consecutive_rejects");
}

//+------------------------------------------------------------------+
void OnTimer()
{
   if(!g_enabled) return;

   //--- Kill switch is also honoured when ticks stop arriving.
   if(SpikeKillActive(InpKillFile) && !g_halted)
   {
      SpikeLog("KILL_TRIGGERED", SpikeS("source", "timer") + " " +
               SpikeI("open_positions", OwnPositionCount()));
      Halt("kill_file");
      return;
   }

   //--- Time stop must fire even in a quiet market.
   if(!g_halted)
   {
      const ulong ticket = OwnPositionTicket();
      if(ticket != 0) ManageOpenPosition(ticket);
   }
}

//+------------------------------------------------------------------+
void OnTradeTransaction(const MqlTradeTransaction &transaction,
                        const MqlTradeRequest &request,
                        const MqlTradeResult &result)
{
   if(transaction.type != TRADE_TRANSACTION_DEAL_ADD) return;
   if(transaction.symbol != _Symbol) return;

   if(!HistoryDealSelect(transaction.deal)) return;
   if(HistoryDealGetInteger(transaction.deal, DEAL_MAGIC) != InpMagic) return;

   const long   entry  = HistoryDealGetInteger(transaction.deal, DEAL_ENTRY);
   const double price  = HistoryDealGetDouble(transaction.deal, DEAL_PRICE);
   const double volume = HistoryDealGetDouble(transaction.deal, DEAL_VOLUME);
   const double profit = HistoryDealGetDouble(transaction.deal, DEAL_PROFIT);
   const double swap   = HistoryDealGetDouble(transaction.deal, DEAL_SWAP);
   const double comm   = HistoryDealGetDouble(transaction.deal, DEAL_COMMISSION);
   const ulong  pos_id = (ulong)HistoryDealGetInteger(transaction.deal, DEAL_POSITION_ID);

   if(entry == DEAL_ENTRY_IN)
   {
      SpikeLog("FILL",
         SpikeI("ticket", (long)pos_id) + " " +
         SpikeD("price", price, g_spec.digits) + " " +
         SpikeD("volume", volume, 2) + " " +
         SpikeI("deal", (long)transaction.deal));

      if(PositionSelectByTicket(pos_id))
      {
         SpikeLog("PROTECTION",
            SpikeI("ticket", (long)pos_id) + " " +
            SpikeD("sl", PositionGetDouble(POSITION_SL), g_spec.digits) + " " +
            SpikeD("tp", PositionGetDouble(POSITION_TP), g_spec.digits));
      }
      return;
   }

   if(entry == DEAL_ENTRY_OUT)
   {
      const double net = profit + swap + comm;
      g_gross_today += net;
      g_last_close_time = TimeTradeServer();

      if(net >= 0.0) { g_wins_today++;   g_consec_losses = 0; }
      else           { g_losses_today++; g_consec_losses++; }

      SpikeLog("POSITION_CLOSE",
         SpikeI("ticket", (long)pos_id) + " " +
         SpikeD("price", price, g_spec.digits) + " " +
         SpikeD("profit", profit, 2) + " " +
         SpikeD("commission", comm, 2) + " " +
         SpikeD("swap", swap, 2) + " " +
         SpikeD("net", net, 2) + " " +
         SpikeI("trades_today", g_trades_today) + " " +
         SpikeD("net_today", g_gross_today, 2));

      const int completed = g_wins_today + g_losses_today;
      if(completed == 1 || completed == 10 || completed == 50 || completed == 100)
      {
         SpikeLog("MILESTONE",
            SpikeI("completed_trades", completed) + " " +
            SpikeI("wins", g_wins_today) + " " +
            SpikeI("losses", g_losses_today) + " " +
            SpikeD("net", g_gross_today, 2));
      }

      if(g_consec_losses >= InpMaxConsecLosses)
      {
         g_pause_until = TimeTradeServer() + InpPauseMinutes * 60;
         SpikeLog("LIMIT_HIT", "limit=max_consecutive_losses " +
                  SpikeI("value", g_consec_losses) + " " +
                  SpikeI("pause_minutes", InpPauseMinutes));
         g_consec_losses = 0;
      }
   }
}
//+------------------------------------------------------------------+
