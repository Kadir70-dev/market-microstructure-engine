"use client";

import Link from "next/link";
import type { HealthPayload, OverviewPayload } from "@/lib/api-types";
import { Card } from "@/components/ui";
import { PanelAction, StatusBadge } from "@/components/layout";

const capabilities = [
  { name: "Market Data", detail: "30-second mid-price collection", glyph: "◇" },
  { name: "Signal Generation", detail: "Momentum and volatility regimes", glyph: "⌁" },
  { name: "Confidence Analysis", detail: "Confidence and trade-quality scores", glyph: "◈" },
  { name: "Calibration", detail: "Look-ahead-safe fwd-60s analysis", glyph: "◎" },
  { name: "Backtesting", detail: "Read-only C++ evaluation harness", glyph: "↯" },
  { name: "Model Diagnostics", detail: "Walk-forward baseline evaluation", glyph: "∆" },
  { name: "Research Reports", detail: "Hermes daily markdown reports", glyph: "≡" },
  { name: "SQLite Pipeline", detail: "Immutable analytical ground truth", glyph: "▱" },
];

const futureCapabilities = ["Order-book analytics", "True cycle latency", "Positions and execution", "Portfolio risk controls"];

const architecture = [
  { label: "Market feed", detail: "Read-only file export" },
  { label: "C++ engine", detail: "Indicators · validation" },
  { label: "SQLite", detail: "Ticks · signals · quality" },
  { label: "Analytics", detail: "Backtest · model · Hermes" },
  { label: "API", detail: "Read-only Next.js routes" },
  { label: "Dashboard", detail: "Observability terminal" },
];

const stack = ["C++17", "Next.js 14", "React 18", "TypeScript", "SQLite", "Tailwind CSS", "Recharts"];

const matrix = [
  { status: "Implemented", tone: "healthy" as const, items: ["C++ collection and persistence", "Signal quality and calibration", "Look-ahead-safe backtest harness", "Read-only dashboard and API", "Model diagnostics", "Hermes research reports"] },
  { status: "Experimental", tone: "synthetic" as const, items: ["ML baseline research pipeline", "MT5 / Wine file-export integration (unverified on this host)"] },
  { status: "Future", tone: "unavailable" as const, items: ["Order-flow and depth analytics", "True engine-cycle latency", "Execution, positions, and portfolio risk"] },
];

export function PortfolioHero({ overview, health, lastUpdated }: { overview: OverviewPayload | null; health: HealthPayload | null; lastUpdated: Date | null }) {
  const warning = Boolean(health?.staleFlagsPresent || health?.perSymbol.some((item) => item.frozen));
  return (
    <section aria-labelledby="portfolio-title" className="relative overflow-hidden border border-edge/80 bg-gradient-to-br from-panel2 via-panel to-terminal-950 px-5 py-6 shadow-terminal sm:px-7 sm:py-8 lg:px-9">
      <div className="pointer-events-none absolute -right-24 -top-24 h-64 w-64 rounded-full bg-accent/[0.045] blur-3xl" />
      <div className="relative grid gap-7 xl:grid-cols-[minmax(0,1fr)_460px] xl:items-end">
        <div>
          <div className="flex flex-wrap items-center gap-2"><StatusBadge tone={overview?.source === "live" ? "live" : "demo"} pulse={overview?.source === "live"}>{overview?.source === "live" ? "Live" : overview?.source === "demo" ? "Demo" : "Connecting"}</StatusBadge><PanelAction>Read-only research</PanelAction></div>
          <p className="mt-6 text-[9px] font-semibold uppercase tracking-[0.28em] text-accent/70">Institutional AI Research Platform</p>
          <h1 id="portfolio-title" className="mt-2 max-w-4xl text-2xl font-semibold tracking-[-0.03em] text-slate-50 sm:text-3xl lg:text-[2.5rem] lg:leading-[1.05]">Market Microstructure Engine</h1>
          <p className="mt-4 max-w-2xl text-xs leading-6 text-slate-500 sm:text-sm">A research-first market intelligence system built around immutable SQLite ground truth, look-ahead-safe evaluation, explicit provenance, and a hard boundary against order execution.</p>
          <div className="mt-6 flex flex-wrap gap-x-6 gap-y-2 text-[9px] uppercase tracking-[0.12em] text-slate-600"><span>Hot path · 30s collection</span><span>Cold path · read-only analysis</span><span>Timezone · UTC</span></div>
        </div>
        <div className="grid grid-cols-2 border-l border-t border-edge/70 sm:grid-cols-4 xl:grid-cols-2">
          <HeroFact label="Build status" value="Verified" badge={<StatusBadge tone="healthy">Passing</StatusBadge>} />
          <HeroFact label="Engine status" value={health ? warning ? "Attention" : "Operational" : "Checking"} badge={<StatusBadge tone={warning ? "warning" : health ? "healthy" : "neutral"}>{warning ? "Warning" : health ? "Healthy" : "Checking"}</StatusBadge>} />
          <HeroFact label="Data source" value={overview?.source === "live" ? "engine.db" : overview?.source === "demo" ? "Synthetic demo" : "Connecting"} badge={<StatusBadge tone={overview?.source === "live" ? "live" : "demo"}>{overview?.source || "Pending"}</StatusBadge>} />
          <HeroFact label="Version" value="Dashboard 0.1.0" badge={<PanelAction>API v1.0</PanelAction>} />
        </div>
      </div>
      <div className="relative mt-5 border-t border-edge/60 pt-3 text-[9px] uppercase tracking-[0.12em] text-slate-700">Last synchronized · {lastUpdated ? `${Math.max(0, Math.floor((Date.now() - lastUpdated.getTime()) / 1000))}s ago` : "awaiting first response"}</div>
    </section>
  );
}

function HeroFact({ label, value, badge }: { label: string; value: string; badge: React.ReactNode }) {
  return <div className="min-h-28 border-b border-r border-edge/70 bg-terminal-950/20 p-3.5 transition-colors hover:bg-white/[0.018]"><p className="text-[8px] uppercase tracking-[0.17em] text-slate-700">{label}</p><p className="mt-2 text-[11px] font-medium text-slate-300">{value}</p><div className="mt-3">{badge}</div></div>;
}

export function PortfolioShowcase() {
  return (
    <section aria-labelledby="capabilities-title" className="space-y-5 pt-2">
      <div className="border-b border-edge/60 pb-3"><p className="text-[9px] uppercase tracking-[0.2em] text-accent/70">System profile</p><h2 id="capabilities-title" className="mt-1 text-lg font-semibold tracking-tight text-slate-100">Engineering capability overview</h2><p className="mt-1 text-[10px] leading-4 text-slate-600">Verified and implemented capabilities are separated explicitly from engine-blocked future work.</p></div>

      <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">{capabilities.map((item) => <article key={item.name} className="group border border-edge/70 bg-panel/75 p-4 shadow-terminal-inset transition-all duration-200 hover:-translate-y-px hover:border-accent/25 hover:bg-panel2/80"><div className="flex items-center justify-between"><span className="text-lg text-accent/70" aria-hidden="true">{item.glyph}</span><StatusBadge tone="healthy">Implemented</StatusBadge></div><h3 className="mt-5 text-xs font-semibold text-slate-200">{item.name}</h3><p className="mt-2 text-[10px] leading-4 text-slate-600">{item.detail}</p></article>)}</div>

      <Card title="Coming in Future Versions" subtitle="Unavailable today; each item requires new engine data or an explicit execution-phase decision." action={<StatusBadge tone="unavailable">Unavailable</StatusBadge>}><div className="grid gap-2 sm:grid-cols-2 xl:grid-cols-4">{futureCapabilities.map((item) => <div key={item} className="flex items-center gap-2 border border-dashed border-edge/70 bg-terminal-950/20 px-3 py-3 text-[10px] text-slate-600"><span className="text-slate-700">◇</span>{item}</div>)}</div></Card>

      <div className="grid gap-3.5 xl:grid-cols-[1.35fr_.65fr]">
        <Card title="System architecture" subtitle="Existing hot path and read-only analytical delivery path only." action={<PanelAction>Current state</PanelAction>}><div className="grid gap-2 md:grid-cols-6">{architecture.map((stage, index) => <div key={stage.label} className="relative border border-edge/70 bg-terminal-950/25 px-3 py-4 text-center transition-colors hover:border-accent/25">{index < architecture.length - 1 && <span className="absolute -right-2.5 top-1/2 z-10 hidden -translate-y-1/2 text-accent/50 md:block">→</span>}<p className="text-[9px] font-semibold uppercase tracking-[0.12em] text-slate-300">{stage.label}</p><p className="mt-2 text-[9px] leading-4 text-slate-700">{stage.detail}</p></div>)}</div></Card>
        <Card title="Technical stack" subtitle="Technologies present in the current repository." action={<PanelAction>Verified</PanelAction>}><div className="flex flex-wrap gap-2">{stack.map((item) => <span key={item} className="border border-edge bg-terminal-950/35 px-3 py-2 text-[9px] font-semibold uppercase tracking-[0.1em] text-slate-400 transition-colors hover:border-accent/30 hover:text-accent">{item}</span>)}</div><Link href="https://github.com/Kadir70-dev/market-microstructure-engine" target="_blank" rel="noreferrer" className="mt-5 inline-flex items-center gap-2 text-[9px] uppercase tracking-[0.12em] text-accent/70 hover:text-accent focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-4 focus-visible:outline-accent">View repository <span aria-hidden="true">↗</span></Link></Card>
      </div>

      <Card title="Feature maturity matrix" subtitle="Classification follows CURRENT_STATUS.md; experimental does not mean production-ready." action={<PanelAction>2026-07-27 audit</PanelAction>}><div className="overflow-x-auto"><table className="w-full min-w-[760px] border-collapse text-left"><thead><tr className="border-b border-edge text-[8px] uppercase tracking-[0.16em] text-slate-700"><th className="w-40 px-3 py-3">Maturity</th><th className="px-3 py-3">Current scope</th><th className="w-64 px-3 py-3">Portfolio interpretation</th></tr></thead><tbody>{matrix.map((row) => <tr key={row.status} className="border-b border-edge/60 align-top transition-colors hover:bg-white/[0.012]"><td className="px-3 py-4"><StatusBadge tone={row.tone}>{row.status}</StatusBadge></td><td className="px-3 py-4"><ul className="grid gap-x-6 gap-y-2 sm:grid-cols-2">{row.items.map((item) => <li key={item} className="flex gap-2 text-[10px] leading-4 text-slate-500"><span className="text-accent/45">·</span>{item}</li>)}</ul></td><td className="px-3 py-4 text-[10px] leading-5 text-slate-600">{row.status === "Implemented" ? "Verified functionality available in the repository today." : row.status === "Experimental" ? "Research or integration work with explicit validation limits." : "Not implemented and never represented as live functionality."}</td></tr>)}</tbody></table></div></Card>
    </section>
  );
}
