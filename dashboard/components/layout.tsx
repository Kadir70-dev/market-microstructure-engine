"use client";

import type { ReactNode } from "react";

export type StatusTone = "live" | "demo" | "synthetic" | "unavailable" | "healthy" | "warning" | "neutral";

const statusStyles: Record<StatusTone, string> = {
  live: "border-bull/30 bg-bull/[0.08] text-bull",
  demo: "border-accent/30 bg-accent/[0.08] text-accent",
  synthetic: "border-violet-400/30 bg-violet-400/[0.08] text-violet-300",
  unavailable: "border-slate-700 bg-slate-800/30 text-slate-600",
  healthy: "border-bull/25 bg-bull/[0.06] text-bull",
  warning: "border-amber-400/30 bg-amber-400/[0.08] text-amber-300",
  neutral: "border-edge bg-panel2/60 text-slate-500",
};

export function StatusBadge({ tone, children, pulse = false, title }: { tone: StatusTone; children: ReactNode; pulse?: boolean; title?: string }) {
  return (
    <span title={title} className={`inline-flex h-6 items-center gap-1.5 whitespace-nowrap border px-2 text-[9px] font-semibold uppercase tracking-[0.12em] ${statusStyles[tone]}`}>
      <span className={`h-1.5 w-1.5 rounded-full bg-current ${pulse ? "animate-pulse" : "opacity-70"}`} />
      {children}
    </span>
  );
}

export function PanelAction({ children, title }: { children: ReactNode; title?: string }) {
  return <span title={title} className="inline-flex h-6 items-center border border-edge/70 bg-terminal-950/40 px-2 text-[9px] uppercase tracking-[0.1em] text-slate-600">{children}</span>;
}

export function KpiRow({ children }: { children: ReactNode }) {
  return <div className="grid grid-cols-2 gap-2.5 md:grid-cols-3 xl:grid-cols-6">{children}</div>;
}

export function SectionGrid({ children, columns = 2 }: { children: ReactNode; columns?: 2 | 3 }) {
  return <div className={`grid gap-3.5 ${columns === 3 ? "lg:grid-cols-2 xl:grid-cols-3" : "xl:grid-cols-2"}`}>{children}</div>;
}

export function SectionHeading({ eyebrow, title, description }: { eyebrow: string; title: string; description: string }) {
  return <div className="flex flex-col gap-1 border-b border-edge/50 pb-2.5 sm:flex-row sm:items-end sm:justify-between"><div><p className="text-[8px] font-semibold uppercase tracking-[0.2em] text-accent/60">{eyebrow}</p><h2 className="mt-1 text-xs font-semibold uppercase tracking-[0.1em] text-slate-300">{title}</h2></div><p className="max-w-xl text-[10px] leading-4 text-slate-700 sm:text-right">{description}</p></div>;
}

export function ContextNote({ label = "Interpretation", children }: { label?: string; children: ReactNode }) {
  return <div className="mt-3 flex gap-3 border-l-2 border-accent/25 bg-terminal-950/25 px-3 py-2.5"><span className="shrink-0 text-[8px] font-semibold uppercase tracking-[0.15em] text-accent/60">{label}</span><p className="text-[10px] leading-4 text-slate-600">{children}</p></div>;
}
