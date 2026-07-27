"use client";

import Link from "next/link";
import { usePathname, useRouter } from "next/navigation";
import { useEffect, useState } from "react";
import { StatusBadge } from "@/components/layout";
import { LiveDataProvider, useSharedLiveData } from "@/components/live-context";

const primaryNav = [
  { href: "/", label: "Dashboard", glyph: "▦", shortcut: "1" },
  { href: "/prediction", label: "Prediction", glyph: "⌁", shortcut: "2" },
  { href: "/reports", label: "Reports", glyph: "≡", shortcut: "3" },
];

const futureNav = [
  { label: "Markets", glyph: "◇" },
  { label: "Order Book", glyph: "≣" },
  { label: "Risk", glyph: "△" },
  { label: "Analytics", glyph: "⌗" },
  { label: "Execution", glyph: "▷" },
];

function NavLink({ href, label, glyph, shortcut, compact = false }: { href: string; label: string; glyph: string; shortcut: string; compact?: boolean }) {
  const pathname = usePathname();
  const active = pathname === href;
  return (
    <Link href={href} aria-current={active ? "page" : undefined} title={`${label} · Alt+${shortcut}`} className={`group flex h-terminal-row items-center gap-3 border-l-2 text-terminal-sm transition-colors focus-visible:outline focus-visible:outline-2 focus-visible:outline-inset focus-visible:outline-accent ${compact ? "justify-center px-2" : "px-4"} ${active ? "border-accent bg-accent/[0.08] text-slate-100" : "border-transparent text-slate-500 hover:bg-white/[0.025] hover:text-slate-200"}`}>
      <span className={`w-4 text-center text-base ${active ? "text-accent" : "text-slate-600 group-hover:text-slate-400"}`}>{glyph}</span>
      {!compact && <><span>{label}</span><kbd className="ml-auto hidden text-[8px] font-normal text-slate-700 xl:inline">⌥{shortcut}</kbd></>}
      {active && !compact && <span className="h-1 w-1 rounded-full bg-accent shadow-[0_0_8px_#38bdf8]" />}
    </Link>
  );
}

function TerminalStatusRows() {
  const { overview, health } = useSharedLiveData();
  const source = overview.data?.source;
  const unhealthy = Boolean(health.error || health.data?.staleFlagsPresent || health.data?.perSymbol.some((item) => item.frozen));
  const statusLabel = health.loading ? "Checking" : unhealthy ? "Warning" : "Healthy";
  const marketLabel = health.loading ? "Checking" : !health.data?.nTicks ? "Unavailable" : unhealthy ? "Warning" : "Observing";

  return (
    <div className="flex min-w-max items-center divide-x divide-edge/60">
      <StatusCell label="Session"><UtcClock /></StatusCell>
      <StatusCell label="Data source"><StatusBadge tone={source === "live" ? "live" : "demo"} pulse={source === "live"} title={overview.data?.sourceReason || undefined}>{source === "live" ? "Live" : source === "demo" ? "Demo" : "Checking"}</StatusBadge></StatusCell>
      <StatusCell label="Engine status"><StatusBadge tone={unhealthy ? "warning" : "healthy"}>{statusLabel}</StatusBadge></StatusCell>
      <StatusCell label="Last update"><UpdatedTimer lastUpdated={overview.lastUpdated} /></StatusCell>
      <StatusCell label="Market status"><StatusBadge tone={marketLabel === "Warning" ? "warning" : marketLabel === "Unavailable" ? "unavailable" : "neutral"} title="DB-derived observation status; not an exchange-open indicator">{marketLabel}</StatusBadge></StatusCell>
      <div className="px-3 py-1.5"><StatusBadge tone="warning">Trading disabled</StatusBadge></div>
    </div>
  );
}

function StatusCell({ label, children }: { label: string; children: React.ReactNode }) {
  return <div className="flex h-10 items-center gap-2 px-3"><span className="text-[8px] uppercase tracking-[0.16em] text-slate-700">{label}</span>{children}</div>;
}

function UtcClock() {
  const [now, setNow] = useState<Date | null>(null);
  useEffect(() => { setNow(new Date()); const timer = window.setInterval(() => setNow(new Date()), 1000); return () => window.clearInterval(timer); }, []);
  return <span className="text-[10px] tabular-nums text-slate-400">UTC {now ? now.toISOString().slice(11, 19) : "--:--:--"}</span>;
}

function UpdatedTimer({ lastUpdated }: { lastUpdated: Date | null }) {
  const [, tick] = useState(0);
  useEffect(() => {
    const timer = window.setInterval(() => tick((value) => value + 1), 1000);
    return () => window.clearInterval(timer);
  }, []);
  return <span className="whitespace-nowrap text-[10px] tabular-nums text-slate-500">{lastUpdated ? `${Math.max(0, Math.floor((Date.now() - lastUpdated.getTime()) / 1000))}s ago` : "Updating…"}</span>;
}

function TerminalShellContent({ children }: { children: React.ReactNode }) {
  const [collapsed, setCollapsed] = useState(false);
  const router = useRouter();
  const { overview } = useSharedLiveData();
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (!event.altKey || event.ctrlKey || event.metaKey) return;
      const item = primaryNav.find((entry) => entry.shortcut === event.key);
      if (item) { event.preventDefault(); router.push(item.href); }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [router]);
  return (
    <div className="min-h-screen bg-terminal-950 bg-terminal-grid bg-[size:32px_32px] text-slate-200">
      <aside className={`fixed inset-y-0 left-0 z-30 hidden border-r border-edge/80 bg-terminal-900/95 shadow-terminal backdrop-blur-xl transition-[width] duration-200 md:flex md:flex-col ${collapsed ? "w-16" : "w-56"}`}>
        <Link href="/" className={`flex h-16 items-center border-b border-edge/80 ${collapsed ? "justify-center px-2" : "gap-3 px-4"}`}>
          <span className="grid h-8 w-8 place-items-center border border-accent/30 bg-accent/[0.08] text-lg font-bold text-accent shadow-[inset_0_0_16px_rgba(56,189,248,.05)]">◧</span>
          {!collapsed && <span className="min-w-0"><span className="block text-xs font-semibold tracking-[0.08em] text-slate-100">MME TERMINAL</span><span className="block text-[9px] uppercase tracking-[0.18em] text-slate-600">Research System</span></span>}
        </Link>
        {!collapsed && <div className="px-4 pb-2 pt-5 text-[9px] font-semibold uppercase tracking-[0.2em] text-slate-700">Workspace</div>}
        <nav className={`space-y-0.5 ${collapsed ? "pt-4" : ""}`}>{primaryNav.map((item) => <NavLink key={item.href} {...item} compact={collapsed} />)}</nav>
        <div className="mx-4 my-4 border-t border-edge/60" />
        {!collapsed && <div className="px-4 pb-2 text-[9px] font-semibold uppercase tracking-[0.2em] text-slate-700">Coming online later</div>}
        <div className="space-y-0.5">{futureNav.map((item) => <div key={item.label} aria-disabled="true" title={`${item.label} · unavailable in the current read-only system`} className={`flex h-terminal-row cursor-not-allowed items-center border-l-2 border-transparent text-terminal-sm text-slate-700 ${collapsed ? "justify-center px-2" : "gap-3 px-4"}`}><span className="w-4 text-center text-base">{item.glyph}</span>{!collapsed && <><span>{item.label}</span><span className="ml-auto text-[8px] uppercase tracking-wider text-slate-800">Unavailable</span></>}</div>)}</div>
        <div className="mt-auto border-t border-edge/70"><button type="button" onClick={() => setCollapsed((value) => !value)} aria-expanded={!collapsed} aria-label={collapsed ? "Expand sidebar" : "Collapse sidebar"} className="flex h-10 w-full items-center justify-center text-slate-600 transition-colors hover:bg-white/[0.025] hover:text-accent focus-visible:outline focus-visible:outline-2 focus-visible:outline-inset focus-visible:outline-accent">{collapsed ? "›" : "‹  Collapse rail"}</button>{!collapsed && <div className="border-t border-edge/50 p-4"><p className="text-[9px] uppercase tracking-[0.16em] text-slate-700">Read-only environment</p><p className="mt-1.5 text-[10px] leading-4 text-slate-600">No broker connection<br />No execution path</p></div>}</div>
      </aside>

      <div className={`transition-[padding] duration-200 ${collapsed ? "md:pl-16" : "md:pl-56"}`}>
        <header className="sticky top-0 z-20 border-b border-edge/80 bg-terminal-950/95 backdrop-blur-xl">
          <div className="flex h-12 items-center justify-between px-3 sm:px-5 lg:px-6"><div className="flex min-w-0 items-center gap-3"><Link href="/" className="text-lg font-bold text-accent md:hidden">◧</Link><div><p className="truncate text-[11px] font-semibold uppercase tracking-[0.16em] text-slate-300">Market Microstructure Engine</p><p className="hidden text-[9px] uppercase tracking-[0.14em] text-slate-700 sm:block">Institutional research workspace</p></div></div><span className="text-[9px] uppercase tracking-[0.16em] text-slate-700">Read only · v1.0</span></div>
          <div className="overflow-x-auto border-t border-edge/60 bg-terminal-900/65 [scrollbar-width:none]"><TerminalStatusRows /></div>
        </header>
        <main className="mx-auto w-full max-w-[1600px] px-3 pb-20 pt-4 sm:px-5 sm:pt-5 lg:px-6 lg:pt-6">{children}</main>
        <footer className="mx-auto w-full max-w-[1600px] border-t border-edge/50 px-4 py-5 text-[9px] uppercase tracking-[0.1em] text-slate-700 sm:px-6"><div className="flex flex-col gap-3 sm:flex-row sm:flex-wrap sm:items-center sm:justify-between"><div className="flex flex-wrap gap-x-5 gap-y-2"><span>Build · Verified</span><span>API · v1.0</span><span>Dashboard · 0.1.0</span><span>Data · {overview.data?.source === "live" ? "Live SQLite" : overview.data?.source === "demo" ? "Demo" : "Connecting"}</span></div><a href="https://github.com/Kadir70-dev/market-microstructure-engine" target="_blank" rel="noreferrer" className="text-slate-600 transition-colors hover:text-accent focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-4 focus-visible:outline-accent">Kadir70-dev / market-microstructure-engine ↗</a></div><p className="mt-3 text-[8px] tracking-[0.12em] text-slate-800">Read-only analytics · no broker connection · no order routing · UTC</p></footer>
      </div>

      <nav aria-label="Mobile workspace navigation" className="fixed inset-x-0 bottom-0 z-30 grid grid-cols-3 border-t border-edge bg-terminal-900/95 pb-[env(safe-area-inset-bottom)] backdrop-blur-xl md:hidden">{primaryNav.map((item) => <NavLink key={item.href} {...item} />)}</nav>
    </div>
  );
}

export function TerminalShell({ children }: { children: React.ReactNode }) {
  return <LiveDataProvider><TerminalShellContent>{children}</TerminalShellContent></LiveDataProvider>;
}
