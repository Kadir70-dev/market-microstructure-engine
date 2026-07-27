import type { ApiErrorCode, ErrorPayload } from "@/lib/api-types";
export class ApiError extends Error {
  constructor(public readonly code: ApiErrorCode, message: string, public readonly status: number) {
    super(message); this.name = "ApiError";
  }
}
function isErrorPayload(value: unknown): value is ErrorPayload {
  if (!value || typeof value !== "object" || !("error" in value)) return false;
  const error = (value as { error?: unknown }).error;
  return Boolean(error && typeof error === "object" && "code" in error && "message" in error);
}
export async function fetchJson<T>(path: string): Promise<T> {
  const response = await fetch(path, { cache: "no-store" });
  let body: unknown;
  try { body = await response.json(); }
  catch { throw new ApiError("internal_error", `Invalid JSON response from ${path}`, response.status); }
  if (!response.ok || isErrorPayload(body)) {
    throw new ApiError(isErrorPayload(body) ? body.error.code : "internal_error", isErrorPayload(body) ? body.error.message : `Request failed (${response.status})`, response.status);
  }
  return body as T;
}
