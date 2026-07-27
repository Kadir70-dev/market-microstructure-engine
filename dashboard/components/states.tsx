"use client";
import { Card } from "@/components/ui";
import { StatusBadge } from "@/components/layout";
export function LoadingPanel({ label = "Loading data…" }: { label?: string }) {
  return <Card className="min-h-40" aria-busy="true"><div className="animate-pulse space-y-4"><div className="flex items-center justify-between"><div className="h-2.5 w-32 bg-slate-700/50" /><div className="h-5 w-16 border border-edge bg-panel2" /></div><div className="h-24 border border-edge/40 bg-panel2/60" /><div className="h-2 w-2/3 bg-slate-800" /></div><span className="sr-only">{label}</span></Card>;
}
export function ErrorPanel({ message, retry }: { message: string; retry: () => void }) {
  return <Card className="border-bear/40" action={<StatusBadge tone="warning">Warning</StatusBadge>}><p className="text-sm font-semibold text-bear">Unable to load data</p><p className="mt-2 text-xs leading-5 text-slate-500">{message}</p><button onClick={retry} className="mt-4 border border-edge bg-panel2 px-3 py-2 text-[10px] font-semibold uppercase tracking-[0.12em] text-slate-300 transition-colors hover:border-accent hover:text-accent focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-accent">Retry request</button></Card>;
}
export function EmptyPanel({ reason }: { reason: string }) {
  return <div className="relative grid min-h-36 place-items-center overflow-hidden border border-dashed border-edge/90 bg-terminal-950/25 px-5 py-8 text-center"><div className="absolute inset-0 bg-terminal-grid bg-[size:18px_18px] opacity-30" /><div className="relative"><span className="mx-auto grid h-8 w-8 place-items-center border border-edge bg-panel/70 text-slate-600">◇</span><p className="mt-3 text-[10px] uppercase tracking-[0.08em] text-slate-600">{reason}</p><div className="mx-auto mt-3 h-px w-12 bg-edge" /></div></div>;
}

export function PanelLoading({ label = "Loading panel…" }: { label?: string }) {
  return <div className="animate-pulse space-y-3 py-4" aria-busy="true"><div className="h-24 border border-edge/40 bg-panel2/50" /><div className="h-2 w-1/2 bg-slate-800" /><span className="sr-only">{label}</span></div>;
}

export function PanelError({ message, retry }: { message: string; retry: () => void }) {
  return <div className="border border-bear/25 bg-bear/[0.035] p-4"><div className="flex items-center gap-2"><StatusBadge tone="warning">Warning</StatusBadge><span className="text-[10px] uppercase tracking-[0.1em] text-slate-500">Panel unavailable</span></div><p className="mt-3 text-xs leading-5 text-slate-500">{message}</p><button type="button" onClick={retry} aria-label="Retry loading this panel" className="mt-3 border border-edge bg-terminal-950/40 px-3 py-2 text-[9px] font-semibold uppercase tracking-[0.12em] text-slate-400 transition-colors hover:border-accent hover:text-accent focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-accent">Retry panel</button></div>;
}
