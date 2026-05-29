import type { Metadata } from "next";
import Link from "next/link";
import "./globals.css";

export const metadata: Metadata = {
  title: "Market Microstructure Engine — Dashboard",
  description:
    "Read-only observability for the Market Microstructure Engine: signal analytics, confidence calibration, hypothetical equity, feed health, and Hermes reports.",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en">
      <body className="font-mono">
        <header className="border-b border-edge bg-panel/60 backdrop-blur sticky top-0 z-10">
          <div className="max-w-7xl mx-auto px-4 py-3 flex items-center gap-6">
            <Link href="/" className="flex items-center gap-2">
              <span className="text-accent text-lg font-bold">◧</span>
              <span className="font-semibold text-slate-100">
                Market Microstructure Engine
              </span>
            </Link>
            <nav className="flex gap-4 text-sm text-slate-400">
              <Link href="/" className="hover:text-accent">
                Dashboard
              </Link>
              <Link href="/prediction" className="hover:text-accent">
                Prediction
              </Link>
              <Link href="/reports" className="hover:text-accent">
                Hermes Reports
              </Link>
              <a
                href="https://github.com/Kadir70-dev/market-microstructure-engine"
                target="_blank"
                rel="noreferrer"
                className="hover:text-accent"
              >
                GitHub ↗
              </a>
            </nav>
            <span className="ml-auto text-xs px-2 py-1 rounded bg-bear/15 text-bear border border-bear/30">
              LIVE TRADING DISABLED
            </span>
          </div>
        </header>
        <main className="max-w-7xl mx-auto px-4 py-6">{children}</main>
        <footer className="max-w-7xl mx-auto px-4 py-8 text-xs text-slate-500">
          Read-only analytics over <code>engine.db</code>. The dashboard never
          writes to the database and never places trades.
        </footer>
      </body>
    </html>
  );
}
