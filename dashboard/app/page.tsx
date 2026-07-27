"use client";

import {
  CalibrationChart,
  ConfidenceHistogram,
  EquityCurveChart,
  GradeDonut,
  PriceChart,
  SignalMixChart,
} from "@/components/charts";
import { Card, DemoBanner, Stat } from "@/components/ui";
import { EmptyPanel, PanelError, PanelLoading } from "@/components/states";
import type { EquityPayload, PricesPayload, SignalsPayload } from "@/lib/api-types";
import { useLiveData } from "@/lib/useLiveData";
import { KpiRow, PanelAction, SectionGrid, SectionHeading, StatusBadge } from "@/components/layout";
import { useSharedLiveData } from "@/components/live-context";
import { PortfolioHero, PortfolioShowcase } from "@/components/portfolio";

function fmtDate(ts: number | null): string {
  if (ts === null) return "—";
  const d = new Date(ts * 1000);
  return d.toISOString().replace("T", " ").slice(0, 16) + " UTC";
}

export default function DashboardPage() {
  const { overview: overviewQuery, health: healthQuery } = useSharedLiveData();
  const pricesQuery = useLiveData<PricesPayload>("/api/prices");
  const signalsQuery = useLiveData<SignalsPayload>("/api/signals");
  const equityQuery = useLiveData<EquityPayload>("/api/equity");
  const ov = overviewQuery.data;
  const prices = pricesQuery.data;
  const sig = signalsQuery.data;
  const eq = equityQuery.data;
  const hl = healthQuery.data;

  const totalDirectional = ov ? (ov.momentum.Bullish || 0) + (ov.momentum.Bearish || 0) : null;
  const gradeCount = sig ? Object.values(sig.grades).reduce((sum, count) => sum + count, 0) : 0;
  const confidenceCount = sig ? sig.confidenceHistogram.reduce((sum, bin) => sum + bin.count, 0) : 0;

  return (
    <div className="space-y-4 sm:space-y-5">
      <PortfolioHero overview={ov} health={hl} lastUpdated={overviewQuery.lastUpdated} />
      {ov && <DemoBanner source={ov.source} sourceReason={ov.sourceReason} />}

      {/* KPI row */}
      <SectionHeading eyebrow="01 · Snapshot" title="Collection overview" description="Counts and quality indicators from the current engine dataset window." />
      {overviewQuery.loading && !ov ? <PanelLoading label="Loading collection overview…" /> : overviewQuery.error ? <PanelError message={overviewQuery.error.message} retry={() => void overviewQuery.refresh()} /> : ov ? <KpiRow>
        <Stat label="Symbols" value={ov.symbols.length} hint={ov.symbols.join(" · ")} />
        <Stat label="Ticks" value={ov.nTicks.toLocaleString()} tone="accent" />
        <Stat label="Signals" value={ov.nSignals.toLocaleString()} />
        <Stat
          label="Directional"
          value={totalDirectional!.toLocaleString()}
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
      </KpiRow> : <EmptyPanel reason="engine has not collected overview data yet" />}

      {ov && <p className="border-y border-edge/40 py-2 text-[10px] uppercase tracking-[0.06em] text-slate-600">
        Window: {fmtDate(ov.firstTs)} → {fmtDate(ov.lastTs)} ·{" "}
        {ov.source === "live" ? `source: ${ov.dbPath}` : "source: synthetic demo session"}
      </p>}

      {/* Prices + Equity */}
      <SectionHeading eyebrow="02 · Market & research" title="Price context and hypothetical outcome" description="Mid-price normalization beside the explicitly hypothetical forward-return diagnostic." />
      <SectionGrid>
        <Card
          title="Price (normalized to 100 at session start)"
          subtitle="Multi-symbol overlay — relative move, scale-invariant"
          action={<PanelAction title="Mid-price snapshots collected every 30 seconds">Mid · 30s</PanelAction>}
        >
          {pricesQuery.loading && !prices ? <PanelLoading label="Loading price series…" /> : pricesQuery.error ? <PanelError message={pricesQuery.error.message} retry={() => void pricesQuery.refresh()} /> : prices && Object.values(prices).some((points) => points.length) ? <PriceChart series={prices} /> : <EmptyPanel reason="engine has not collected price data yet" />}
        </Card>
        <Card
          title="Hypothetical equity curve"
          subtitle="Signals are NOT traded — cumulative fwd-60s directional return, gross vs net of round-trip cost"
          action={<StatusBadge tone="synthetic">Research</StatusBadge>}
        >
          {equityQuery.loading && !eq ? <PanelLoading label="Loading hypothetical equity diagnostic…" /> : equityQuery.error ? <PanelError message={equityQuery.error.message} retry={() => void equityQuery.refresh()} /> : eq && eq.points.length ? <><EquityCurveChart points={eq.points} />
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
          </> : <EmptyPanel reason="no hypothetical forward observations are computable yet" />}
        </Card>
      </SectionGrid>

      {/* Signal analytics */}
      <SectionHeading eyebrow="03 · Signals" title="Composition and confidence" description="Signal direction, trade-quality grades, and the reported confidence-score distribution." />
      <SectionGrid columns={3}>
        <Card title="Momentum mix by symbol" action={<PanelAction>Signals</PanelAction>}>
          {signalsQuery.loading && !sig ? <PanelLoading label="Loading signal mix…" /> : signalsQuery.error ? <PanelError message={signalsQuery.error.message} retry={() => void signalsQuery.refresh()} /> : sig && Object.keys(sig.perSymbol).length ? <SignalMixChart perSymbol={sig.perSymbol} /> : <EmptyPanel reason="engine has not collected signals yet" />}
        </Card>
        <Card title="Trade-quality grade distribution" subtitle="A≥75 · B≥50 · C≥25 · D<25" action={<PanelAction>Quality</PanelAction>}>
          {signalsQuery.loading && !sig ? <PanelLoading label="Loading grade distribution…" /> : signalsQuery.error ? <PanelError message={signalsQuery.error.message} retry={() => void signalsQuery.refresh()} /> : sig && gradeCount > 0 ? <GradeDonut grades={sig.grades} /> : <EmptyPanel reason="no trade-quality grades are available in this window" />}
        </Card>
        <Card title="Confidence distribution" subtitle="histogram of 0–100 score" action={<PanelAction>Score</PanelAction>}>
          {signalsQuery.loading && !sig ? <PanelLoading label="Loading confidence distribution…" /> : signalsQuery.error ? <PanelError message={signalsQuery.error.message} retry={() => void signalsQuery.refresh()} /> : sig && confidenceCount > 0 ? <ConfidenceHistogram bins={sig.confidenceHistogram} /> : <EmptyPanel reason="no confidence scores are available in this window" />}
        </Card>
      </SectionGrid>

      {/* Calibration + Health */}
      <SectionHeading eyebrow="04 · Validation" title="Calibration and feed condition" description="Look-ahead-safe directional calibration beside DB-derived feed freshness and frozen-feed checks." />
      <SectionGrid>
        <Card
          title="Confidence calibration"
          subtitle="Does higher confidence → higher realized fwd-60s accuracy? (look-ahead-safe, stale/neutral excluded)"
          action={<PanelAction>Fwd 60s</PanelAction>}
        >
          {signalsQuery.loading && !sig ? <PanelLoading label="Loading calibration…" /> : signalsQuery.error ? <PanelError message={signalsQuery.error.message} retry={() => void signalsQuery.refresh()} /> : sig && sig.calibration.some((band) => band.n > 0) ? <CalibrationChart calibration={sig.calibration} /> : <EmptyPanel reason="no look-ahead-safe calibration observations are computable yet" />}
        </Card>
        <Card title="Feed health" subtitle="per-symbol last tick, age, and frozen-feed detection" action={hl ? <StatusBadge tone={hl.perSymbol.some((item) => item.frozen) ? "warning" : "healthy"}>{hl.perSymbol.some((item) => item.frozen) ? "Warning" : "Healthy"}</StatusBadge> : <StatusBadge tone="neutral">Checking</StatusBadge>}>
          {healthQuery.loading && !hl ? <PanelLoading label="Loading feed health…" /> : healthQuery.error ? <PanelError message={healthQuery.error.message} retry={() => void healthQuery.refresh()} /> : hl && hl.perSymbol.length ? <div className="overflow-x-auto">
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
                        <StatusBadge tone="warning">Warning</StatusBadge>
                      ) : (
                        <StatusBadge tone="healthy">Healthy</StatusBadge>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div> : <EmptyPanel reason="engine has not collected feed-health data yet" />}
          {hl && <p className="text-xs text-slate-500 mt-3">
            Staleness mirrors <code>validation/isStale</code>: ≥5 byte-identical
            ticks ⇒ frozen. {hl.staleFlagsPresent ? "Stale flags present in this window." : "No stale flags in this window."}
          </p>}
        </Card>
      </SectionGrid>

      <Card
        title="Why this dashboard exists"
        action={<StatusBadge tone="neutral">Read only</StatusBadge>}
        className="text-sm text-slate-400 leading-relaxed"
      >
        Read-only observability over the engine&apos;s SQLite ground truth. Every
        number here is re-derived from raw ticks using the{" "}
        <span className="text-slate-200">same look-ahead-safe gates and cost model</span>{" "}
        as the C++ backtest harness — no fabricated returns, no peeking at future
        prices. The equity curve is explicitly hypothetical: this system{" "}
        <span className="text-bear">does not place trades</span>.
      </Card>
      <PortfolioShowcase />
    </div>
  );
}
