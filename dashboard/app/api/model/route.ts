import { handle } from "@/lib/api";
import { loadModelResults } from "@/lib/model";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return handle(() => loadModelResults());
}
