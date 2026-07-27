"use client";

import {
  FeatureImportanceChart,
  NetOfCostChart,
  ReliabilityChart,
} from "@/components/charts";
import { Card, DemoBanner, Stat } from "@/components/ui";
import { EmptyPanel, PanelError, PanelLoading } from "@/components/states";
import type { ModelPayload } from "@/lib/api-types";
import { useLiveData } from "@/lib/useLiveData";
import { ContextNote, KpiRow, PanelAction, SectionGrid, SectionHeading, StatusBadge } from "@/components/layout";
import { useSharedLiveData } from "@/components/live-context";

export default function PredictionPage() {
  const modelQuery = useLiveData<ModelPayload>("/api/model");
  const { overview: overviewQuery } = useSharedLiveData();
  const provenance = overviewQuery.data ? <DemoBanner source={overviewQuery.data.source} sourceReason={overviewQuery.data.sourceReason} /> : overviewQuery.error ? <PanelError message={overviewQuery.error.message} retry={() => void overviewQuery.refresh()} /> : <PanelLoading label="Loading data provenance…" />;

  if (modelQuery.loading && !modelQuery.data) return <div className="space-y-4 sm:space-y-5">{provenance}<PredictionHeading /><PanelLoading label="Loading model analytics…" /></div>;
  if (modelQuery.error) return <div className="space-y-4 sm:space-y-5">{provenance}<PredictionHeading /><PanelError message={modelQuery.error.message} retry={() => void modelQuery.refresh()} /></div>;
  if (!modelQuery.data) return <div className="space-y-4 sm:space-y-5">{provenance}<PredictionHeading /><EmptyPanel reason="model analytics are not available yet" /></div>;
  const m = modelQuery.data;

  if (m.trainable === false) {
    return (
      <div className="space-y-4 sm:space-y-5">
        {provenance}
        <PredictionHeading badge={<StatusBadge tone="unavailable">Unavailable</StatusBadge>} />
        <Card>
          <p className="text-bear font-semibold">Live data is not trainable yet.</p>
          <p className="text-sm text-slate-400 mt-2">{m.reason}</p>
          <p className="text-sm text-slate-500 mt-3">{m.verdict}</p>
        </Card>
      </div>
    );
  }

  const met = m.metrics!;
  const aboveChance = met.p_value !== null && met.p_value < 0.05 && met.auc > 0.5;
  const net0 = m.net_of_cost!.threshold_curve[0];

  return (
    <div className="space-y-4 sm:space-y-5">
      {provenance}
      <PredictionHeading badge={<StatusBadge tone={m.source === "live" ? "live" : "synthetic"}>{m.source === "live" ? "Live" : "Synthetic"}</StatusBadge>} />
      <p className="text-[11px] leading-5 text-slate-600">
        {m.model} · {m.n_oos?.toLocaleString()} out-of-sample predictions ·{" "}
        {m.n_features} features. Probabilities, not binary calls; evaluated net of
        cost.
      </p>

      {/* Verdict banner */}
      <div
        className={`border px-4 py-3 text-xs leading-5 ${
          net0.net_pct > 0
            ? "border-bull/30 bg-bull/10 text-bull"
            : "border-amber-500/30 bg-amber-500/10 text-amber-300"
        }`}
      >
        <strong>Honest verdict:</strong> {m.verdict}
      </div>

      {/* KPIs */}
      <SectionHeading eyebrow="01 · Model diagnostics" title="Out-of-sample summary" description="Reported model quality and net-of-cost diagnostics; values are unchanged from the model results." />
      <KpiRow>
        <Stat
          label="OOS AUC"
          value={met.auc.toFixed(3)}
          tone={aboveChance ? "accent" : "default"}
          hint={`baseline ${met.baseline_auc}`}
        />
        <Stat
          label="Permutation AUC"
          value={`${met.permutation_auc_mean.toFixed(3)}`}
          hint={`±${met.permutation_auc_std.toFixed(3)} (null)`}
        />
        <Stat
          label="p-value"
          value={met.p_value === null ? "—" : met.p_value.toFixed(3)}
          tone={aboveChance ? "bull" : "bear"}
          hint={aboveChance ? "above chance" : "≈ chance"}
        />
        <Stat label="Precision / Recall" value={`${met.precision.toFixed(2)}/${met.recall.toFixed(2)}`} />
        <Stat label="Brier" value={met.brier.toFixed(3)} hint="lower = better" />
        <Stat
          label="Net of cost"
          value={`${net0.net_pct.toFixed(1)}%`}
          tone={net0.net_pct > 0 ? "bull" : "bear"}
          hint={`gross ${net0.gross_pct.toFixed(1)}%`}
        />
      </KpiRow>

      <SectionGrid>
        <Card
          title="Calibration (reliability diagram)"
          subtitle="predicted P(up) vs observed frequency — on the dashed diagonal = well-calibrated"
          action={<PanelAction>OOS</PanelAction>}
        >
          <ReliabilityChart points={m.calibration!} />
          <ContextNote>Observed frequency is plotted against predicted probability; the dashed diagonal is the existing perfect-calibration reference.</ContextNote>
        </Card>
        <Card
          title="Net-of-cost vs selectivity"
          subtitle="gross edge exists; round-trip costs push net negative at every trade threshold"
          action={<StatusBadge tone="warning">Costs</StatusBadge>}
        >
          <NetOfCostChart curve={m.net_of_cost!.threshold_curve} />
          <ContextNote>The chart compares the reported gross and net curves after applying the existing round-trip cost model.</ContextNote>
        </Card>
      </SectionGrid>

      <Card
        title="Feature importance"
        subtitle="standardized Logistic Regression coefficients (green = ↑P(up), red = ↓)"
        action={<PanelAction>Top 10</PanelAction>}
      >
        <FeatureImportanceChart features={m.feature_importance!} />
        <ContextNote>Signed standardized coefficients show association with predicted direction; this panel does not make a causal claim.</ContextNote>
      </Card>

      <Card className="text-sm leading-relaxed text-slate-400" action={<StatusBadge tone="synthetic">Methodology</StatusBadge>}>
        <strong className="text-slate-200">Method.</strong> Walk-forward
        (expanding, time-ordered) Logistic Regression on the engine&apos;s own
        features. Labels are look-ahead-safe forward-60s direction. The
        permutation test shuffles labels {`>`}25× to establish the null AUC. The
        net-of-cost curve applies the per-symbol round-trip cost model.{" "}
        {m.source === "synthetic" && (
          <span className="text-slate-500">
            Computed on a disclosed synthetic session because live{" "}
            <code>engine.db</code> isn&apos;t trainable yet
            {m.real_db_reason ? ` (${m.real_db_reason})` : ""}; the pipeline
            switches to live data automatically once enough is collected. This is
            methodology, not a performance claim.
          </span>
        )}
      </Card>
    </div>
  );
}

function PredictionHeading({ badge }: { badge?: React.ReactNode }) {
  return <div className="flex flex-col gap-3 border-b border-edge/60 pb-4 sm:flex-row sm:items-end sm:justify-between"><div><p className="text-[9px] uppercase tracking-[0.2em] text-accent/70">Model research</p><h1 className="mt-1 text-lg font-semibold tracking-tight text-slate-100 sm:text-xl">Prediction — next-direction probability</h1></div>{badge}</div>;
}
