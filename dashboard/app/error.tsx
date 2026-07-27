"use client";
import { useEffect } from "react";
import { ErrorPanel } from "@/components/states";
export default function ErrorBoundary({ error, reset }: { error: Error & { digest?: string }; reset: () => void }) {
  useEffect(() => { console.error(error); }, [error]);
  return <ErrorPanel message={error.message || "The page could not be loaded."} retry={reset} />;
}
