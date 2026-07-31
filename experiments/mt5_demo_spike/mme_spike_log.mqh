//+------------------------------------------------------------------+
//| mme_spike_log.mqh                                                |
//| Structured single-line logging for the throwaway demo spike.     |
//| One tag per event so the Experts tab can be grepped or exported. |
//+------------------------------------------------------------------+
#ifndef MME_SPIKE_LOG_MQH
#define MME_SPIKE_LOG_MQH

void SpikeLog(const string tag, const string body)
{
   PrintFormat("%s %s", tag, body);
}

string SpikeD(const string key, const double value, const int digits)
{
   return StringFormat("%s=%s", key, DoubleToString(value, digits));
}

string SpikeI(const string key, const long value)
{
   return StringFormat("%s=%I64d", key, value);
}

string SpikeS(const string key, const string value)
{
   return StringFormat("%s=%s", key, value);
}

//--- Human-readable retcode so a rejection is never just a number.
string SpikeRetcodeText(const uint retcode)
{
   switch(retcode)
   {
      case TRADE_RETCODE_DONE:             return "DONE";
      case TRADE_RETCODE_DONE_PARTIAL:     return "DONE_PARTIAL";
      case TRADE_RETCODE_PLACED:           return "PLACED";
      case TRADE_RETCODE_REQUOTE:          return "REQUOTE";
      case TRADE_RETCODE_PRICE_CHANGED:    return "PRICE_CHANGED";
      case TRADE_RETCODE_PRICE_OFF:        return "PRICE_OFF";
      case TRADE_RETCODE_TIMEOUT:          return "TIMEOUT";
      case TRADE_RETCODE_INVALID:          return "INVALID";
      case TRADE_RETCODE_INVALID_VOLUME:   return "INVALID_VOLUME";
      case TRADE_RETCODE_INVALID_PRICE:    return "INVALID_PRICE";
      case TRADE_RETCODE_INVALID_STOPS:    return "INVALID_STOPS";
      case TRADE_RETCODE_INVALID_FILL:     return "INVALID_FILL";
      case TRADE_RETCODE_TRADE_DISABLED:   return "TRADE_DISABLED";
      case TRADE_RETCODE_MARKET_CLOSED:    return "MARKET_CLOSED";
      case TRADE_RETCODE_NO_MONEY:         return "NO_MONEY";
      case TRADE_RETCODE_TOO_MANY_REQUESTS:return "TOO_MANY_REQUESTS";
      case TRADE_RETCODE_FROZEN:           return "FROZEN";
      case TRADE_RETCODE_CONNECTION:       return "CONNECTION";
      case TRADE_RETCODE_LIMIT_ORDERS:     return "LIMIT_ORDERS";
      case TRADE_RETCODE_LIMIT_VOLUME:     return "LIMIT_VOLUME";
      case TRADE_RETCODE_REJECT:           return "REJECT";
      default:                             return "UNMAPPED";
   }
}

#endif // MME_SPIKE_LOG_MQH
