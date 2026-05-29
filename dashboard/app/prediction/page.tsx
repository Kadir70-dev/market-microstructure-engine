import {
  FeatureImportanceChart,
  NetOfCostChart,
  ReliabilityChart,
} from "@/components/charts";
import { Card, Stat } from "@/components/ui";
import { loadModelResults } from "@/lib/model";

export const dynamic = "force-dynamic";

export default function PredictionPage() {
  const m = loadModelResults();

  if (m.trainable === false) {
    return (
      <div>
        <h1 className="text-lg font-semibold text-slate-100 mb-1">
          Prediction — next-direction probability
        </h1>
        <Card className="mt-4">
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
    <div>
      <div className="flex items-baseline justify-between mb-1">
        <h1 className="text-lg font-semibold text-slate-100">
          Prediction — next-direction probability
        </h1>
        <span
          className={`text-xs px-2 py-1 rounded border ${
            m.source === "live"
              ? "bg-bull/15 text-bull border-bull/30"
              : "bg-accent/15 text-accent border-accent/30"
          }`}
        >
          {m.source === "live" ? "LIVE DATA" : "SYNTHETIC (live data not yet trainable)"}
        </span>
      </div>
      <p className="text-sm text-slate-500 mb-5">
        {m.model} · {m.n_oos?.toLocaleString()} out-of-sample predictions ·{" "}
        {m.n_features} features. Probabilities, not binary calls; evaluated net of
        cost.
      </p>

      {/* Verdict banner */}
      <div
        className={`mb-6 rounded-lg border px-4 py-3 text-sm ${
          net0.net_pct > 0
            ? "border-bull/30 bg-bull/10 text-bull"
            : "border-amber-500/30 bg-amber-500/10 text-amber-300"
        }`}
      >
        <strong>Honest verdict:</strong> {m.verdict}
      </div>

      {/* KPIs */}
      <div className="grid grid-cols-2 md:grid-cols-4 lg:grid-cols-6 gap-3 mb-6">
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
      </div>

      <div className="grid lg:grid-cols-2 gap-4 mb-4">
        <Card
          title="Calibration (reliability diagram)"
          subtitle="predicted P(up) vs observed frequency — on the dashed diagonal = well-calibrated"
        >
          <ReliabilityChart points={m.calibration!} />
        </Card>
        <Card
          title="Net-of-cost vs selectivity"
          subtitle="gross edge exists; round-trip costs push net negative at every trade threshold"
        >
          <NetOfCostChart curve={m.net_of_cost!.threshold_curve} />
        </Card>
      </div>

      <Card
        title="Feature importance"
        subtitle="standardized Logistic Regression coefficients (green = ↑P(up), red = ↓)"
      >
        <FeatureImportanceChart features={m.feature_importance!} />
      </Card>

      <Card className="mt-4 text-sm text-slate-400 leading-relaxed">
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
