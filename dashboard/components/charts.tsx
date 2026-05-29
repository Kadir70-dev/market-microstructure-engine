"use client";

import {
  Bar,
  BarChart,
  CartesianGrid,
  Cell,
  Legend,
  Line,
  LineChart,
  Pie,
  PieChart,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";

const SYMBOL_COLORS: Record<string, string> = {
  "EUR/USD": "#38bdf8",
  "XAU/USD": "#fbbf24",
  USO: "#a78bfa",
};
const GRID = "#1f2937";
const AXIS = "#64748b";

function hhmm(ts: number): string {
  const d = new Date(ts * 1000);
  return `${String(d.getUTCHours()).padStart(2, "0")}:${String(
    d.getUTCMinutes(),
  ).padStart(2, "0")}`;
}

const tooltipStyle = {
  backgroundColor: "#0a0e14",
  border: "1px solid #1f2937",
  borderRadius: 8,
  fontSize: 12,
};

// ---- Normalized multi-symbol price chart (indexed to 100 at start) -------
export function PriceChart({
  series,
}: {
  series: Record<string, { ts: number; price: number }[]>;
}) {
  const symbols = Object.keys(series);
  const tsSet = new Set<number>();
  const norm: Record<string, Map<number, number>> = {};
  for (const sym of symbols) {
    const arr = series[sym];
    const first = arr[0]?.price || 1;
    const m = new Map<number, number>();
    for (const p of arr) {
      m.set(p.ts, (p.price / first) * 100);
      tsSet.add(p.ts);
    }
    norm[sym] = m;
  }
  const rows = Array.from(tsSet)
    .sort((a, b) => a - b)
    .map((ts) => {
      const row: Record<string, number> = { ts };
      for (const sym of symbols) {
        const v = norm[sym].get(ts);
        if (v !== undefined) row[sym] = Number(v.toFixed(3));
      }
      return row;
    });

  return (
    <ResponsiveContainer width="100%" height={260}>
      <LineChart data={rows} margin={{ top: 5, right: 10, bottom: 0, left: -10 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="3 3" />
        <XAxis
          dataKey="ts"
          tickFormatter={hhmm}
          stroke={AXIS}
          fontSize={11}
          minTickGap={40}
        />
        <YAxis
          stroke={AXIS}
          fontSize={11}
          domain={["auto", "auto"]}
          tickFormatter={(v) => v.toFixed(1)}
        />
        <Tooltip
          contentStyle={tooltipStyle}
          labelFormatter={(ts) => `${hhmm(Number(ts))} UTC`}
        />
        <Legend wrapperStyle={{ fontSize: 12 }} />
        {symbols.map((sym) => (
          <Line
            key={sym}
            type="monotone"
            dataKey={sym}
            stroke={SYMBOL_COLORS[sym] || "#38bdf8"}
            dot={false}
            strokeWidth={1.5}
            connectNulls
          />
        ))}
      </LineChart>
    </ResponsiveContainer>
  );
}

// ---- Per-symbol momentum mix (grouped bars) ------------------------------
export function SignalMixChart({
  perSymbol,
}: {
  perSymbol: Record<string, { bull: number; bear: number; neutral: number }>;
}) {
  const data = Object.entries(perSymbol).map(([symbol, v]) => ({
    symbol,
    Bullish: v.bull,
    Bearish: v.bear,
    Neutral: v.neutral,
  }));
  return (
    <ResponsiveContainer width="100%" height={220}>
      <BarChart data={data} margin={{ top: 5, right: 10, bottom: 0, left: -10 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="3 3" />
        <XAxis dataKey="symbol" stroke={AXIS} fontSize={11} />
        <YAxis stroke={AXIS} fontSize={11} />
        <Tooltip contentStyle={tooltipStyle} />
        <Legend wrapperStyle={{ fontSize: 12 }} />
        <Bar dataKey="Bullish" fill="#22c55e" />
        <Bar dataKey="Bearish" fill="#ef4444" />
        <Bar dataKey="Neutral" fill="#64748b" />
      </BarChart>
    </ResponsiveContainer>
  );
}

// ---- Grade distribution (donut) ------------------------------------------
const GRADE_COLORS: Record<string, string> = {
  A: "#22c55e",
  B: "#38bdf8",
  C: "#fbbf24",
  D: "#ef4444",
};
export function GradeDonut({ grades }: { grades: Record<string, number> }) {
  const data = Object.entries(grades)
    .filter(([, v]) => v > 0)
    .map(([grade, value]) => ({ name: grade, value }));
  return (
    <ResponsiveContainer width="100%" height={220}>
      <PieChart>
        <Pie
          data={data}
          dataKey="value"
          nameKey="name"
          innerRadius={45}
          outerRadius={75}
          paddingAngle={2}
          label={(e) => `${e.name}: ${e.value}`}
          fontSize={12}
        >
          {data.map((d) => (
            <Cell key={d.name} fill={GRADE_COLORS[d.name] || "#64748b"} />
          ))}
        </Pie>
        <Tooltip contentStyle={tooltipStyle} />
      </PieChart>
    </ResponsiveContainer>
  );
}

// ---- Confidence histogram -------------------------------------------------
export function ConfidenceHistogram({
  bins,
}: {
  bins: { label: string; count: number }[];
}) {
  return (
    <ResponsiveContainer width="100%" height={220}>
      <BarChart data={bins} margin={{ top: 5, right: 10, bottom: 0, left: -10 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="3 3" />
        <XAxis dataKey="label" stroke={AXIS} fontSize={10} angle={-30} dy={8} height={40} />
        <YAxis stroke={AXIS} fontSize={11} />
        <Tooltip contentStyle={tooltipStyle} />
        <Bar dataKey="count" fill="#38bdf8" />
      </BarChart>
    </ResponsiveContainer>
  );
}

// ---- Confidence calibration (accuracy vs band, ref line at 50%) ----------
export function CalibrationChart({
  calibration,
}: {
  calibration: { band: string; n: number; accuracy: number | null }[];
}) {
  const data = calibration.map((c) => ({
    band: c.band,
    accuracy: c.accuracy ?? 0,
    n: c.n,
  }));
  return (
    <ResponsiveContainer width="100%" height={220}>
      <BarChart data={data} margin={{ top: 5, right: 10, bottom: 0, left: -10 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="3 3" />
        <XAxis dataKey="band" stroke={AXIS} fontSize={10} />
        <YAxis stroke={AXIS} fontSize={11} domain={[0, 100]} unit="%" />
        <Tooltip
          contentStyle={tooltipStyle}
          formatter={(v: number, _n, p: any) =>
            [`${v.toFixed(1)}%  (N=${p.payload.n})`, "fwd-60s accuracy"]
          }
        />
        <ReferenceLine
          y={50}
          stroke="#64748b"
          strokeDasharray="4 4"
          label={{ value: "coin flip", fill: "#64748b", fontSize: 10, position: "right" }}
        />
        <Bar dataKey="accuracy" fill="#a78bfa" radius={[4, 4, 0, 0]} />
      </BarChart>
    </ResponsiveContainer>
  );
}

// ---- Hypothetical equity curve (gross vs net) ----------------------------
export function EquityCurveChart({
  points,
}: {
  points: { ts: number; gross: number; net: number }[];
}) {
  return (
    <ResponsiveContainer width="100%" height={260}>
      <LineChart data={points} margin={{ top: 5, right: 10, bottom: 0, left: -10 }}>
        <CartesianGrid stroke={GRID} strokeDasharray="3 3" />
        <XAxis dataKey="ts" tickFormatter={hhmm} stroke={AXIS} fontSize={11} minTickGap={40} />
        <YAxis stroke={AXIS} fontSize={11} unit="%" tickFormatter={(v) => v.toFixed(2)} />
        <Tooltip
          contentStyle={tooltipStyle}
          labelFormatter={(ts) => `${hhmm(Number(ts))} UTC`}
          formatter={(v: number) => `${v.toFixed(3)}%`}
        />
        <ReferenceLine y={0} stroke="#64748b" />
        <Legend wrapperStyle={{ fontSize: 12 }} />
        <Line type="monotone" dataKey="gross" name="Gross" stroke="#38bdf8" dot={false} strokeWidth={1.5} />
        <Line type="monotone" dataKey="net" name="Net of cost" stroke="#22c55e" dot={false} strokeWidth={1.5} />
      </LineChart>
    </ResponsiveContainer>
  );
}
