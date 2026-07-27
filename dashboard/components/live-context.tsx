"use client";

import { createContext, useContext, type ReactNode } from "react";
import type { HealthPayload, OverviewPayload } from "@/lib/api-types";
import { useLiveData } from "@/lib/useLiveData";

export interface LiveQuery<T> {
  data: T | null;
  error: Error | null;
  loading: boolean;
  lastUpdated: Date | null;
  refresh: () => Promise<void>;
}

interface LiveContextValue {
  overview: LiveQuery<OverviewPayload>;
  health: LiveQuery<HealthPayload>;
}

const LiveContext = createContext<LiveContextValue | null>(null);

export function LiveDataProvider({ children }: { children: ReactNode }) {
  const overview = useLiveData<OverviewPayload>("/api/overview");
  const health = useLiveData<HealthPayload>("/api/health");
  return <LiveContext.Provider value={{ overview, health }}>{children}</LiveContext.Provider>;
}

export function useSharedLiveData(): LiveContextValue {
  const value = useContext(LiveContext);
  if (!value) throw new Error("useSharedLiveData must be used within LiveDataProvider");
  return value;
}
