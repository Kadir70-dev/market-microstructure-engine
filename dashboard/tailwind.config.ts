import type { Config } from "tailwindcss";

const config: Config = {
  content: [
    "./app/**/*.{ts,tsx}",
    "./components/**/*.{ts,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        // Terminal-ish quant palette.
        ink: "#0a0e14",
        panel: "#111722",
        panel2: "#161d2b",
        edge: "#1f2937",
        bull: "#22c55e",
        bear: "#ef4444",
        neutral: "#64748b",
        accent: "#38bdf8",
      },
      fontFamily: {
        mono: ["ui-monospace", "SFMono-Regular", "Menlo", "monospace"],
      },
    },
  },
  plugins: [],
};

export default config;
