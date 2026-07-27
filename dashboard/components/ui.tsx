"use client";

import React from "react";
import type { ReactNode } from "react";

export function Card({
  title,
  subtitle,
  children,
  action,
  className = "",
}: {
  title?: string;
  subtitle?: string;
  children: React.ReactNode;
  action?: ReactNode;
  className?: string;
}) {
  return (
    <div
      className={`border border-edge/80 bg-gradient-to-b from-panel2/70 to-panel/95 p-3.5 shadow-terminal-inset transition-colors hover:border-slate-700/80 sm:p-4 ${className}`}
    >
      {title && (
        <div className="mb-3 flex min-h-10 items-start justify-between gap-3 border-b border-edge/60 pb-2.5">
          <div className="min-w-0"><h2 className="text-terminal-sm font-semibold uppercase tracking-[0.08em] text-slate-200">{title}</h2>{subtitle && <p className="mt-1 text-[10px] leading-4 text-slate-600">{subtitle}</p>}</div>
          {action && <div className="shrink-0">{action}</div>}
        </div>
      )}
      {children}
    </div>
  );
}

export function Stat({
  label,
  value,
  hint,
  tone = "default",
}: {
  label: string;
  value: string | number;
  hint?: string;
  tone?: "default" | "bull" | "bear" | "accent";
}) {
  const toneCls =
    tone === "bull"
      ? "text-bull"
      : tone === "bear"
        ? "text-bear"
        : tone === "accent"
          ? "text-accent"
          : "text-slate-100";
  return (
    <div className="group relative min-h-[104px] overflow-hidden border border-edge/80 bg-gradient-to-br from-panel2 to-panel p-3.5 shadow-terminal-inset transition-all duration-200 hover:-translate-y-px hover:border-slate-600/80 hover:shadow-[0_12px_30px_rgba(0,0,0,.16)] focus-within:border-accent/40">
      <div className="absolute inset-x-0 top-0 h-px bg-gradient-to-r from-transparent via-slate-500/15 to-transparent" />
      <div className={`absolute bottom-0 left-0 top-0 w-0.5 transition-opacity group-hover:opacity-100 ${tone === "bull" ? "bg-bull" : tone === "bear" ? "bg-bear" : tone === "accent" ? "bg-accent" : "bg-slate-600/50 opacity-30"}`} />
      <div className="text-[9px] font-semibold uppercase tracking-[0.16em] text-slate-600">
        {label}
      </div>
      <div className={`mt-2 text-[1.45rem] font-semibold leading-none tabular-nums tracking-tight ${toneCls}`}>{value}</div>
      {hint && <div className="mt-2 truncate text-[10px] text-slate-600">{hint}</div>}
    </div>
  );
}

export function DemoBanner({ source, sourceReason }: { source: "live" | "demo"; sourceReason: string | null }) {
  if (source === "live") {
    return (
      <div className="mb-4 flex items-center gap-2 border border-bull/20 bg-bull/[0.055] px-3 py-2 text-[10px] uppercase tracking-[0.08em] text-bull">
        <span className="h-1.5 w-1.5 rounded-full bg-bull shadow-[0_0_8px_#22c55e]" /> LIVE · reading directly from <code className="normal-case">engine.db</code> · read-only
      </div>
    );
  }
  return (
    <div className="mb-4 border border-accent/20 bg-accent/[0.055] px-3 py-2 text-[10px] leading-4 text-accent">
      <span className="font-semibold uppercase tracking-[0.08em]">◇ Demo data</span> — a deterministic synthetic session derived with the engine&apos;s
      exact formulas. {sourceReason && <span className="text-sky-200">{sourceReason}. </span>}
      Set <code>DASHBOARD_DB_PATH=../data/engine.db</code> to show
      live data.
    </div>
  );
}
