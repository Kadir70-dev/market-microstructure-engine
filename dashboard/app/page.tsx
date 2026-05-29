import {
  CalibrationChart,
  ConfidenceHistogram,
  EquityCurveChart,
  GradeDonut,
  PriceChart,
  SignalMixChart,
} from "@/components/charts";
import { Card, DemoBanner, Stat } from "@/components/ui";
import {
  confidenceCalibration,
  equityCurve,
  health,
  overview,
  priceSeries,
  signalAnalytics,
} from "@/lib/analytics";
import { loadDataset } from "@/lib/db";

export const dynamic = "force-dynamic";

function fmtDate(ts: number | null): string {
  if (ts === null) return "—";
  const d = new Date(ts * 1000);
  return d.toISOString().replace("T", " ").slice(0, 16) + " UTC";
}

export default function DashboardPage() {
  const ds = loadDataset();
  const ov = overview(ds);
  const prices = priceSeries(ds);
  const sig = signalAnalytics(ds);
  const calib = confidenceCalibration(ds);
  const eq = equityCurve(ds);
  const hl = health(ds);

  const totalDirectional = (ov.momentum.Bullish || 0) + (ov.momentum.Bearish || 0);

  return (
    <div>
      <DemoBanner source={ov.source} />

      {/* KPI row */}
      <div className="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-6 gap-3 mb-6">
        <Stat label="Symbols" value={ov.symbols.length} hint={ov.symbols.join(" · ")} />
        <Stat label="Ticks" value={ov.nTicks.toLocaleString()} tone="accent" />
        <Stat label="Signals" value={ov.nSignals.toLocaleString()} />
        <Stat
          label="Directional"
          value={totalDirectional.toLocaleString()}
          hint={`${ov.momentum.Neutral || 0} neutral`}
        />
        <Stat
          label="Mean confidence"
          value={ov.meanConfidence.toFixed(1)}
          hint="0–100"
        />
        <Stat
          label="Stale signals"
          value={`${ov.stalePct.toFixed(1)}%`}
          tone={ov.stalePct > 30 ? "bear" : "default"}
          hint={`${ov.stale} flagged`}
        />
      </div>

      <p className="text-xs text-slate-500 mb-6">
        Window: {fmtDate(ov.firstTs)} → {fmtDate(ov.lastTs)} ·{" "}
        {ov.source === "live" ? `source: ${ov.dbPath}` : "source: synthetic demo session"}
      </p>

      {/* Prices + Equity */}
      <div className="grid lg:grid-cols-2 gap-4 mb-4">
        <Card
          title="Price (normalized to 100 at session start)"
          subtitle="Multi-symbol overlay — relative move, scale-invariant"
        >
          <PriceChart series={prices} />
        </Card>
        <Card
          title="Hypothetical equity curve"
          subtitle="Signals are NOT traded — cumulative fwd-60s directional return, gross vs net of round-trip cost"
        >
          <EquityCurveChart points={eq.points} />
          {eq.stats.cumGrossPct > 0 && eq.stats.cumNetPct < 0 && (
            <p className="text-xs text-amber-400/90 mt-2 leading-relaxed">
              ⓘ Gross edge is positive but <strong>net is negative</strong>:
              round-trip costs at 30s cadence erase a {eq.stats.hitRate.toFixed(1)}%
              hit rate. This is precisely why the engine{" "}
              <span className="text-slate-300">collects data instead of trading</span>.
            </p>
          )}
          <div className="flex flex-wrap gap-4 mt-3 text-xs">
            <span className="text-slate-400">
              N=<span className="text-slate-200">{eq.stats.n}</span>
            </span>
            <span className="text-slate-400">
              hit rate=
              <span className="text-slate-200">{eq.stats.hitRate.toFixed(1)}%</span>
            </span>
            <span className="text-slate-400">
              gross=
              <span className={eq.stats.cumGrossPct >= 0 ? "text-bull" : "text-bear"}>
                {eq.stats.cumGrossPct.toFixed(3)}%
              </span>
            </span>
            <span className="text-slate-400">
              net=
              <span className={eq.stats.cumNetPct >= 0 ? "text-bull" : "text-bear"}>
                {eq.stats.cumNetPct.toFixed(3)}%
              </span>
            </span>
          </div>
        </Card>
      </div>

      {/* Signal analytics */}
      <div className="grid lg:grid-cols-3 gap-4 mb-4">
        <Card title="Momentum mix by symbol">
          <SignalMixChart perSymbol={sig.perSymbol} />
        </Card>
        <Card title="Trade-quality grade distribution" subtitle="A≥75 · B≥50 · C≥25 · D<25">
          <GradeDonut grades={sig.grades} />
        </Card>
        <Card title="Confidence distribution" subtitle="histogram of 0–100 score">
          <ConfidenceHistogram bins={sig.confidenceHistogram} />
        </Card>
      </div>

      {/* Calibration + Health */}
      <div className="grid lg:grid-cols-2 gap-4 mb-4">
        <Card
          title="Confidence calibration"
          subtitle="Does higher confidence → higher realized fwd-60s accuracy? (look-ahead-safe, stale/neutral excluded)"
        >
          <CalibrationChart calibration={calib} />
        </Card>
        <Card title="Feed health" subtitle="per-symbol last tick, age, and frozen-feed detection">
          <div className="overflow-x-auto">
            <table className="w-full text-sm">
              <thead>
                <tr className="text-slate-500 text-left text-xs border-b border-edge">
                  <th className="py-2">Symbol</th>
                  <th>Ticks</th>
                  <th>Last price</th>
                  <th>Last tick</th>
                  <th>Feed</th>
                </tr>
              </thead>
              <tbody>
                {hl.perSymbol.map((s) => (
                  <tr key={s.symbol} className="border-b border-edge/50">
                    <td className="py-2 text-slate-200">{s.symbol}</td>
                    <td className="text-slate-400">{s.ticks}</td>
                    <td className="text-slate-300">{s.lastPrice ?? "—"}</td>
                    <td className="text-slate-500 text-xs">
                      {s.lastTs
                        ? new Date(s.lastTs * 1000).toISOString().slice(11, 16) + " UTC"
                        : "—"}
                    </td>
                    <td>
                      {s.frozen ? (
                        <span className="text-bear">● frozen</span>
                      ) : (
                        <span className="text-bull">● live</span>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          <p className="text-xs text-slate-500 mt-3">
            Staleness mirrors <code>validation/isStale</code>: ≥5 byte-identical
            ticks ⇒ frozen. {hl.staleFlagsPresent ? "Stale flags present in this window." : "No stale flags in this window."}
          </p>
        </Card>
      </div>

      <Card
        title="Why this dashboard exists"
        className="text-sm text-slate-400 leading-relaxed"
      >
        Read-only observability over the engine&apos;s SQLite ground truth. Every
        number here is re-derived from raw ticks using the{" "}
        <span className="text-slate-200">same look-ahead-safe gates and cost model</span>{" "}
        as the C++ backtest harness — no fabricated returns, no peeking at future
        prices. The equity curve is explicitly hypothetical: this system{" "}
        <span className="text-bear">does not place trades</span>.
      </Card>
    </div>
  );
}
