// Shared response shape for every /api route.
//
// Success payloads are returned verbatim (unwrapped) so existing consumers keep
// working; the contract this file adds is on the *failure* side: an unexpected
// throw must not leak an HTML error page or a 200 with a broken body. Every
// route answers with either its documented JSON payload (200) or
// `{ error: { code, message } }` with a 4xx/5xx status.
//
// Read-only by construction: there are no POST/PUT/DELETE helpers here, and
// there must not be. The dashboard never writes to engine.db.

import { NextResponse } from "next/server";

export interface ApiErrorBody {
  error: {
    code: string;
    message: string;
  };
}

// Analytics are recomputed per request from a short-TTL dataset cache, so
// responses must never be cached by Next, a proxy, or the browser.
const NO_STORE = { "Cache-Control": "no-store" } as const;

export function jsonOk<T>(data: T): NextResponse {
  return NextResponse.json(data, { status: 200, headers: NO_STORE });
}

export function jsonError(
  code: string,
  message: string,
  status = 500,
): NextResponse {
  const body: ApiErrorBody = { error: { code, message } };
  return NextResponse.json(body, { status, headers: NO_STORE });
}

/**
 * Runs a route's payload builder and converts any throw into a 500 with the
 * standard error body. `fn` may return a payload directly, or a NextResponse
 * when the route needs a non-200 status of its own (e.g. 404).
 */
export async function handle<T>(
  fn: () => T | Promise<T>,
): Promise<NextResponse> {
  try {
    const result = await fn();
    if (result instanceof NextResponse) return result;
    return jsonOk(result);
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    console.error("[dashboard/api]", message);
    return jsonError("internal_error", message, 500);
  }
}
