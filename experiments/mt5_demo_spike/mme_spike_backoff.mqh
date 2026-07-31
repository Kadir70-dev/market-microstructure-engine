//+------------------------------------------------------------------+
//| mme_spike_backoff.mqh                                            |
//| Rejection backoff for the demo spike.                            |
//|                                                                   |
//| A broker rejection must never be retried at tick rate. Every      |
//| failed OrderSend feeds this state machine, which imposes a        |
//| growing quiet period and, after a bounded number of consecutive   |
//| failures, refuses to permit any further attempt at all.           |
//|                                                                   |
//| Deliberately free of MT5 calls: 'now' is injected, so the whole   |
//| state machine is deterministic and testable off-terminal. See     |
//| mme_spike_backoff_test.mq5.                                       |
//+------------------------------------------------------------------+
#ifndef MME_SPIKE_BACKOFF_MQH
#define MME_SPIKE_BACKOFF_MQH

struct SpikeRejectState
{
   int      consec;        // consecutive failed OrderSend calls
   datetime block_until;   // no attempt may be made before this server time
};

void SpikeRejectReset(SpikeRejectState &state)
{
   state.consec      = 0;
   state.block_until = 0;
}

//--- A completed order clears the penalty entirely. Only an unbroken run
//--- of failures is evidence that the broker is refusing us.
void SpikeRejectOnSuccess(SpikeRejectState &state)
{
   SpikeRejectReset(state);
}

//--- Linear backoff: the nth consecutive failure blocks for n * base
//--- seconds. Linear rather than exponential because the consecutive-reject
//--- halt already bounds the total; exponential growth would only delay the
//--- operator's feedback without preventing anything extra.
void SpikeRejectOnFailure(SpikeRejectState &state, const datetime now,
                          const int base_seconds)
{
   state.consec++;
   state.block_until = (datetime)(now + base_seconds * state.consec);
}

bool SpikeRejectBlocked(const SpikeRejectState &state, const datetime now)
{
   return(now < state.block_until);
}

//--- Hard stop: the broker has refused this many attempts in a row, so the
//--- cause is not transient and a human needs to look at it.
bool SpikeRejectExhausted(const SpikeRejectState &state, const int max_consec)
{
   return(state.consec >= max_consec);
}

#endif // MME_SPIKE_BACKOFF_MQH
