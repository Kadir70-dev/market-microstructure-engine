import { NextResponse } from "next/server";
import { loadModelResults } from "@/lib/model";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return NextResponse.json(loadModelResults());
}
