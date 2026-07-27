import type { Metadata } from "next";
import { TerminalShell } from "@/components/shell";
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
      <body className="font-mono antialiased"><TerminalShell>{children}</TerminalShell></body>
    </html>
  );
}
