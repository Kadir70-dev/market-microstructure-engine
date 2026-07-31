//+------------------------------------------------------------------+
//| mme_spike_backoff_test.mq5                                       |
//| Deterministic tests for the rejection backoff state machine.     |
//|                                                                   |
//| Run as a Script on any chart. It touches no account state, sends  |
//| no orders and reads no quotes: every 'now' is a synthetic         |
//| timestamp, so the result is identical on any terminal at any      |
//| time. Output goes to the Experts tab, last line PASS or FAIL.     |
//+------------------------------------------------------------------+
#property strict
#property description "Offline tests for mme_spike_backoff.mqh. Sends no orders."

#include "mme_spike_backoff.mqh"

int g_pass = 0;
int g_fail = 0;

void Check(const bool condition, const string name)
{
   if(condition) { g_pass++; PrintFormat("  pass  %s", name); }
   else          { g_fail++; PrintFormat("  FAIL  %s", name); }
}

//--- Fixed synthetic epoch. Nothing here depends on the wall clock.
const datetime T0   = (datetime)1000000;
const int      BASE = 5;
const int      MAXC = 5;

//+------------------------------------------------------------------+
void TestFreshStateAllowsAttempt()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   Check(!SpikeRejectBlocked(s, T0),            "fresh state is not blocked");
   Check(!SpikeRejectExhausted(s, MAXC),        "fresh state is not exhausted");
   Check(s.consec == 0,                         "fresh state has zero consec");
}

//+------------------------------------------------------------------+
void TestFirstFailureBlocksForBase()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   SpikeRejectOnFailure(s, T0, BASE);

   Check(s.consec == 1,                         "first failure sets consec=1");
   Check(SpikeRejectBlocked(s, T0),             "blocked immediately after failure");
   Check(SpikeRejectBlocked(s, T0 + BASE - 1),  "still blocked one second early");
   Check(!SpikeRejectBlocked(s, T0 + BASE),     "unblocks exactly at the deadline");
   Check(!SpikeRejectBlocked(s, T0 + BASE + 1), "stays unblocked after deadline");
}

//+------------------------------------------------------------------+
void TestBackoffGrowsLinearly()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   SpikeRejectOnFailure(s, T0, BASE);
   Check(s.block_until == T0 + BASE,      "1st failure blocks for 1x base");

   SpikeRejectOnFailure(s, T0 + BASE, BASE);
   Check(s.block_until == T0 + BASE + 2 * BASE, "2nd failure blocks for 2x base");

   SpikeRejectOnFailure(s, T0 + 3 * BASE, BASE);
   Check(s.block_until == T0 + 3 * BASE + 3 * BASE, "3rd failure blocks for 3x base");
}

//+------------------------------------------------------------------+
void TestSuccessClearsPenalty()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   SpikeRejectOnFailure(s, T0, BASE);
   SpikeRejectOnFailure(s, T0 + BASE, BASE);
   Check(s.consec == 2,                          "two failures accumulate");

   SpikeRejectOnSuccess(s);
   Check(s.consec == 0,                          "success resets consec");
   Check(!SpikeRejectBlocked(s, T0),             "success clears the block");

   //--- A later failure must start again from the base delay, not resume
   //--- the old penalty. Otherwise one bad patch poisons the whole session.
   SpikeRejectOnFailure(s, T0 + 100, BASE);
   Check(s.block_until == T0 + 100 + BASE,       "failure after success restarts at base");
}

//+------------------------------------------------------------------+
void TestExhaustionThreshold()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   datetime now = T0;
   for(int i = 1; i < MAXC; i++)
   {
      SpikeRejectOnFailure(s, now, BASE);
      now = s.block_until;
      Check(!SpikeRejectExhausted(s, MAXC),
            StringFormat("not exhausted after %d of %d failures", i, MAXC));
   }

   SpikeRejectOnFailure(s, now, BASE);
   Check(SpikeRejectExhausted(s, MAXC),          "exhausted at the configured maximum");
}

//+------------------------------------------------------------------+
//| Regression for the 2026-07-30 incident. The broker rejected every |
//| order (NO_MONEY, then CONNECTION) and the EA retried on every     |
//| tick, producing 275 rejected orders in one session. The guarded   |
//| loop must issue no more than MAXC attempts before halting.        |
//+------------------------------------------------------------------+
void TestTickFloodIsBounded()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   const int ticks_per_second = 10;
   const int seconds          = 600;
   const int total_ticks      = ticks_per_second * seconds;

   int  attempts = 0;
   bool halted   = false;

   for(int i = 0; i < total_ticks && !halted; i++)
   {
      const datetime now = (datetime)(T0 + i / ticks_per_second);

      if(SpikeRejectBlocked(s, now)) continue;

      attempts++;                                   // OrderSend would happen here
      SpikeRejectOnFailure(s, now, BASE);           // ... and the broker refuses

      if(SpikeRejectExhausted(s, MAXC)) halted = true;
   }

   Check(attempts == MAXC,
         StringFormat("flood bounded to %d attempts (was %d unguarded)", MAXC, total_ticks));
   Check(halted, "halts after the maximum consecutive rejections");
   Check(attempts < total_ticks / 100, "attempts are orders of magnitude below tick count");
}

//+------------------------------------------------------------------+
//| A broker that recovers must not leave the EA permanently damped.  |
//+------------------------------------------------------------------+
void TestRecoveryAfterTransientFailures()
{
   SpikeRejectState s;
   SpikeRejectReset(s);

   SpikeRejectOnFailure(s, T0, BASE);
   SpikeRejectOnFailure(s, T0 + BASE, BASE);

   const datetime resumed = s.block_until;
   Check(!SpikeRejectBlocked(s, resumed),         "block expires on schedule");

   SpikeRejectOnSuccess(s);
   Check(!SpikeRejectExhausted(s, MAXC),          "recovery clears exhaustion risk");
   Check(!SpikeRejectBlocked(s, resumed),         "recovered state permits attempts");
}

//+------------------------------------------------------------------+
void OnStart()
{
   Print("mme_spike_backoff tests");

   TestFreshStateAllowsAttempt();
   TestFirstFailureBlocksForBase();
   TestBackoffGrowsLinearly();
   TestSuccessClearsPenalty();
   TestExhaustionThreshold();
   TestTickFloodIsBounded();
   TestRecoveryAfterTransientFailures();

   PrintFormat("%s  %d passed, %d failed",
               (g_fail == 0 ? "PASS" : "FAIL"), g_pass, g_fail);
}
//+------------------------------------------------------------------+
