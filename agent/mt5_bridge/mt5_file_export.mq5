//+------------------------------------------------------------------+
//|  mt5_file_export.mq5                                             |
//|  Read-only tick exporter for the Market Microstructure Engine.   |
//|                                                                  |
//|  WHY THIS EXISTS (Linux + Wine):                                 |
//|  The Python `MetaTrader5` package is Windows-only and does not   |
//|  run natively on Linux. Rather than fight Wine-Python IPC, this  |
//|  Expert Advisor runs INSIDE the MT5 terminal (which runs fine    |
//|  under Wine) and simply writes the latest quotes to a CSV file.  |
//|  The native-Linux engine reads that file. No sockets, no Python  |
//|  under Wine, no live-trading code — by construction.             |
//|                                                                  |
//|  OUTPUT: <terminal>/MQL5/Files/mme_quotes.csv  (overwritten each |
//|  WriteIntervalSec) with one line per symbol:                     |
//|      symbol,epoch_seconds,bid,ask,mid                            |
//|  Under Wine this maps to a normal Linux path, e.g.               |
//|      ~/.mt5/drive_c/Program Files/<Terminal>/MQL5/Files/         |
//|  Point the engine's file provider at that path.                  |
//|                                                                  |
//|  SAFETY: this EA never sends an order. It also logs a warning if |
//|  attached to a non-demo account (mirrors the engine's demo gate).|
//+------------------------------------------------------------------+
#property strict
#property description "Read-only quote exporter (writes CSV for the C++ engine). No trading."

input string InpSymbols       = "EURUSD,XAUUSD,XTIUSD"; // comma-separated broker symbols
input string InpOutFile       = "mme_quotes.csv";       // written under MQL5/Files/
input int    InpWriteInterval = 5;                      // seconds between writes

string g_symbols[];

int OnInit()
{
   // Split the symbol list and ensure each is selected in Market Watch.
   int n = StringSplit(InpSymbols, ',', g_symbols);
   if(n <= 0)
   {
      Print("mme_export: no symbols configured");
      return(INIT_PARAMETERS_INCORRECT);
   }
   for(int i = 0; i < n; i++)
   {
      StringTrimLeft(g_symbols[i]);
      StringTrimRight(g_symbols[i]);
      if(!SymbolSelect(g_symbols[i], true))
         PrintFormat("mme_export: WARN could not select %s", g_symbols[i]);
   }

   // Demo tripwire — log loudly if this is not a demo account.
   long mode = AccountInfoInteger(ACCOUNT_TRADE_MODE);
   if(mode != ACCOUNT_TRADE_MODE_DEMO)
      Print("mme_export: WARNING — account is NOT demo. This EA is read-only, "
            "but you should run data collection on a demo account.");

   EventSetTimer(InpWriteInterval > 0 ? InpWriteInterval : 5);
   Print("mme_export: started, writing ", InpOutFile, " every ",
         InpWriteInterval, "s");
   return(INIT_SUCCEEDED);
}

void OnDeinit(const int reason)
{
   EventKillTimer();
}

void OnTimer()
{
   // Overwrite a single snapshot file each interval. The engine reads the
   // freshest snapshot; if the EA/terminal stalls, the file's mtime stops
   // advancing and the engine's staleness detection flags it.
   int h = FileOpen(InpOutFile, FILE_WRITE | FILE_CSV | FILE_ANSI, ',');
   if(h == INVALID_HANDLE)
   {
      PrintFormat("mme_export: FileOpen failed (%d)", GetLastError());
      return;
   }

   for(int i = 0; i < ArraySize(g_symbols); i++)
   {
      string sym = g_symbols[i];
      MqlTick t;
      if(!SymbolInfoTick(sym, t))
         continue;                      // no quote yet — skip this symbol
      if(t.bid <= 0.0 && t.ask <= 0.0)
         continue;
      double mid = (t.bid + t.ask) / 2.0;
      // epoch seconds, full precision via _Digits-aware formatting
      FileWrite(h, sym, (long)t.time, t.bid, t.ask, mid);
   }
   FileClose(h);
}
//+------------------------------------------------------------------+
