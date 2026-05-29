import { NextResponse } from "next/server";
import { getReport } from "@/lib/reports";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(
  _req: Request,
  { params }: { params: { date: string } },
) {
  const md = getReport(params.date);
  if (md === null) {
    return NextResponse.json({ error: "report not found" }, { status: 404 });
  }
  return NextResponse.json({ date: params.date, markdown: md });
}
