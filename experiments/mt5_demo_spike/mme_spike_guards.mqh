//+------------------------------------------------------------------+
//| mme_spike_guards.mqh                                             |
//| Every safety gate for the demo spike, isolated from strategy      |
//| code so the whole safety surface can be audited on one screen.    |
//|                                                                   |
//| LIVE TRADING IS IMPOSSIBLE HERE. The demo check is a hard         |
//| INIT_FAILED and is re-verified on every tick. There is no input,  |
//| flag or recompile path that bypasses it.                          |
//+------------------------------------------------------------------+
#ifndef MME_SPIKE_GUARDS_MQH
#define MME_SPIKE_GUARDS_MQH

#include "mme_spike_log.mqh"

//--- Resolved once at init, read by the EA thereafter.
struct SpikeSymbolSpec
{
   double point;
   int    digits;
   double volume_min;
   double volume_step;
   double volume_max;
   int    stops_level;
   int    freeze_level;
   int    filling_mode;   // ENUM_ORDER_TYPE_FILLING as int
};

//--- The one and only account/server/symbol this EA will ever touch.
bool SpikeGuardAccount(const long expected_account, const string expected_server)
{
   const long trade_mode = AccountInfoInteger(ACCOUNT_TRADE_MODE);
   if(trade_mode != ACCOUNT_TRADE_MODE_DEMO)
   {
      SpikeLog("INIT_REJECT", "reason=not_demo " + SpikeI("trade_mode", trade_mode));
      return(false);
   }
   const long login = AccountInfoInteger(ACCOUNT_LOGIN);
   if(login != expected_account)
   {
      SpikeLog("INIT_REJECT", "reason=account_mismatch " + SpikeI("actual", login) +
               " " + SpikeI("expected", expected_account));
      return(false);
   }
   const string server = AccountInfoString(ACCOUNT_SERVER);
   if(server != expected_server)
   {
      SpikeLog("INIT_REJECT", "reason=server_mismatch " + SpikeS("actual", server) +
               " " + SpikeS("expected", expected_server));
      return(false);
   }
   return(true);
}

//--- Re-checked every tick: an account switch mid-session halts the EA.
bool SpikeStillDemo(const long expected_account)
{
   if(AccountInfoInteger(ACCOUNT_TRADE_MODE) != ACCOUNT_TRADE_MODE_DEMO) return(false);
   if(AccountInfoInteger(ACCOUNT_LOGIN) != expected_account) return(false);
   return(true);
}

bool SpikeGuardSymbol(const string chart_symbol, const string allowed_symbol)
{
   if(chart_symbol != allowed_symbol)
   {
      SpikeLog("INIT_REJECT", "reason=symbol_not_allowed " + SpikeS("chart", chart_symbol) +
               " " + SpikeS("allowed", allowed_symbol));
      return(false);
   }
   if(!SymbolSelect(chart_symbol, true))
   {
      SpikeLog("INIT_REJECT", "reason=symbol_select_failed " + SpikeS("symbol", chart_symbol));
      return(false);
   }
   return(true);
}

//--- Fixed minimum lot. Never scaled, never derived from any prior outcome.
bool SpikeGuardLots(const string symbol, const double lots, const double hard_cap,
                    SpikeSymbolSpec &spec)
{
   spec.point       = SymbolInfoDouble(symbol, SYMBOL_POINT);
   spec.digits      = (int)SymbolInfoInteger(symbol, SYMBOL_DIGITS);
   spec.volume_min  = SymbolInfoDouble(symbol, SYMBOL_VOLUME_MIN);
   spec.volume_step = SymbolInfoDouble(symbol, SYMBOL_VOLUME_STEP);
   spec.volume_max  = SymbolInfoDouble(symbol, SYMBOL_VOLUME_MAX);
   spec.stops_level = (int)SymbolInfoInteger(symbol, SYMBOL_TRADE_STOPS_LEVEL);
   spec.freeze_level= (int)SymbolInfoInteger(symbol, SYMBOL_TRADE_FREEZE_LEVEL);

   if(spec.point <= 0.0)
   {
      SpikeLog("INIT_REJECT", "reason=bad_point");
      return(false);
   }
   if(lots > hard_cap + 1e-12)
   {
      SpikeLog("INIT_REJECT", "reason=lot_above_hard_cap " + SpikeD("lots", lots, 2) +
               " " + SpikeD("cap", hard_cap, 2));
      return(false);
   }
   if(MathAbs(lots - spec.volume_min) > 1e-12)
   {
      SpikeLog("INIT_REJECT", "reason=lot_not_broker_minimum " + SpikeD("lots", lots, 2) +
               " " + SpikeD("volume_min", spec.volume_min, 2));
      return(false);
   }
   return(true);
}

//--- Resolve a supported filling mode or refuse to start.
bool SpikeGuardFilling(const string symbol, SpikeSymbolSpec &spec)
{
   const int flags = (int)SymbolInfoInteger(symbol, SYMBOL_FILLING_MODE);
   if((flags & SYMBOL_FILLING_FOK) != 0)      spec.filling_mode = (int)ORDER_FILLING_FOK;
   else if((flags & SYMBOL_FILLING_IOC) != 0) spec.filling_mode = (int)ORDER_FILLING_IOC;
   else
   {
      const long exec = SymbolInfoInteger(symbol, SYMBOL_TRADE_EXEMODE);
      if(exec == SYMBOL_TRADE_EXECUTION_MARKET || exec == SYMBOL_TRADE_EXECUTION_EXCHANGE)
         spec.filling_mode = (int)ORDER_FILLING_RETURN;
      else
      {
         SpikeLog("INIT_REJECT", "reason=no_supported_filling_mode " + SpikeI("flags", flags));
         return(false);
      }
   }
   return(true);
}

//--- Broker-side stops must be attachable, or this EA does not trade at all.
bool SpikeGuardStops(const SpikeSymbolSpec &spec, const int sl_points, const int tp_points)
{
   if(sl_points <= 0 || tp_points <= 0)
   {
      SpikeLog("INIT_REJECT", "reason=sl_or_tp_not_configured");
      return(false);
   }
   if(sl_points <= spec.stops_level || tp_points <= spec.stops_level)
   {
      SpikeLog("INIT_REJECT", "reason=sl_tp_inside_stops_level " +
               SpikeI("stops_level", spec.stops_level) + " " +
               SpikeI("sl_points", sl_points) + " " + SpikeI("tp_points", tp_points));
      return(false);
   }
   return(true);
}

bool SpikeGuardAutoTrading()
{
   if(!TerminalInfoInteger(TERMINAL_TRADE_ALLOWED))
   {
      SpikeLog("INIT_REJECT", "reason=terminal_autotrading_disabled action=enable_AutoTrading_button");
      return(false);
   }
   if(!MQLInfoInteger(MQL_TRADE_ALLOWED))
   {
      SpikeLog("INIT_REJECT", "reason=ea_algo_trading_disabled action=tick_Allow_Algo_Trading_in_EA_properties");
      return(false);
   }
   return(true);
}

//--- External kill switch: any human can halt the EA by creating this file
//--- in MQL5\Files. Checked on every tick and on the one-second timer.
bool SpikeKillActive(const string kill_file)
{
   return(FileIsExist(kill_file));
}

#endif // MME_SPIKE_GUARDS_MQH
