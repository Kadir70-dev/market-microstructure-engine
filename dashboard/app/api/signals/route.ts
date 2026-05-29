import { NextResponse } from "next/server";
import { loadDataset } from "@/lib/db";
import { confidenceCalibration, signalAnalytics } from "@/lib/analytics";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  const ds = loadDataset();
  return NextResponse.json({
    ...signalAnalytics(ds),
    calibration: confidenceCalibration(ds),
  });
}
