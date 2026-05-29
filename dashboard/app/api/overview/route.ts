import { NextResponse } from "next/server";
import { loadDataset } from "@/lib/db";
import { overview } from "@/lib/analytics";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  const ds = loadDataset();
  return NextResponse.json(overview(ds));
}
