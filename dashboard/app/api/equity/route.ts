import { handle } from "@/lib/api";
import { loadDataset } from "@/lib/db";
import { equityCurve } from "@/lib/analytics";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return handle(() => equityCurve(loadDataset()));
}
