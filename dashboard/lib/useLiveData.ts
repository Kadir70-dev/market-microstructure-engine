"use client";
import { useCallback, useEffect, useRef, useState } from "react";
import { fetchJson } from "@/lib/client";
export function useLiveData<T>(path: string | null, intervalMs = 10_000) {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState<Error | null>(null);
  const [loading, setLoading] = useState(Boolean(path));
  const [lastUpdated, setLastUpdated] = useState<Date | null>(null);
  const activeRequest = useRef(0);
  const refresh = useCallback(async () => {
    if (!path) return;
    const request = ++activeRequest.current;
    try {
      const next = await fetchJson<T>(path);
      if (request !== activeRequest.current) return;
      setData(next); setError(null); setLastUpdated(new Date());
    } catch (caught) {
      if (request === activeRequest.current) setError(caught instanceof Error ? caught : new Error("Unknown request error"));
    } finally { if (request === activeRequest.current) setLoading(false); }
  }, [path]);
  useEffect(() => {
    setData(null); setError(null); setLoading(Boolean(path)); void refresh();
    if (!path) return;
    const timer = window.setInterval(() => { if (!document.hidden) void refresh(); }, intervalMs);
    const onVisibility = () => { if (!document.hidden) void refresh(); };
    document.addEventListener("visibilitychange", onVisibility);
    return () => { activeRequest.current += 1; window.clearInterval(timer); document.removeEventListener("visibilitychange", onVisibility); };
  }, [path, intervalMs, refresh]);
  return { data, error, loading, lastUpdated, refresh };
}
