import { handle } from "@/lib/api";
import { listReports } from "@/lib/reports";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return handle(() => ({ dates: listReports() }));
}
