import React from "react";

export function Card({
  title,
  subtitle,
  children,
  className = "",
}: {
  title?: string;
  subtitle?: string;
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <div
      className={`bg-panel border border-edge rounded-lg p-4 ${className}`}
    >
      {title && (
        <div className="mb-3">
          <h2 className="text-sm font-semibold text-slate-200">{title}</h2>
          {subtitle && (
            <p className="text-xs text-slate-500 mt-0.5">{subtitle}</p>
          )}
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
    <div className="bg-panel2 border border-edge rounded-lg p-3">
      <div className="text-[11px] uppercase tracking-wide text-slate-500">
        {label}
      </div>
      <div className={`text-2xl font-bold mt-1 ${toneCls}`}>{value}</div>
      {hint && <div className="text-[11px] text-slate-500 mt-1">{hint}</div>}
    </div>
  );
}

export function DemoBanner({ source }: { source: "live" | "demo" }) {
  if (source === "live") {
    return (
      <div className="mb-5 rounded-lg border border-bull/30 bg-bull/10 px-4 py-2 text-sm text-bull">
        ● LIVE — reading directly from <code>engine.db</code> (read-only).
      </div>
    );
  }
  return (
    <div className="mb-5 rounded-lg border border-accent/30 bg-accent/10 px-4 py-2 text-sm text-accent">
      ⚠ DEMO DATA — a deterministic synthetic session derived with the engine&apos;s
      exact formulas. Set <code>DASHBOARD_DB_PATH=../data/engine.db</code> to show
      live data.
    </div>
  );
}
