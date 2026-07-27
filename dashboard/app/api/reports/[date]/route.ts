import { handle, jsonError } from "@/lib/api";
import { getReport } from "@/lib/reports";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(
  _req: Request,
  { params }: { params: { date: string } },
) {
  return handle(() => {
    const md = getReport(params.date);
    if (md === null) {
      return jsonError("not_found", `no report for ${params.date}`, 404);
    }
    return { date: params.date, markdown: md };
  });
}
