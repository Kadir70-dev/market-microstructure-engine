import { handle } from "@/lib/api";
import { loadDataset } from "@/lib/db";
import { confidenceCalibration, signalAnalytics } from "@/lib/analytics";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return handle(() => {
    const ds = loadDataset();
    return {
      ...signalAnalytics(ds),
      calibration: confidenceCalibration(ds),
    };
  });
}
