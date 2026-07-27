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
        terminal: {
          950: "#070a0f",
          900: "#0b1018",
          850: "#0e141e",
          800: "#131b28",
        },
      },
      fontFamily: {
        mono: ["ui-monospace", "SFMono-Regular", "Menlo", "monospace"],
      },
      fontSize: {
        "terminal-xs": ["0.6875rem", { lineHeight: "1rem", letterSpacing: "0.035em" }],
        "terminal-sm": ["0.75rem", { lineHeight: "1.125rem" }],
      },
      spacing: {
        "terminal-row": "2.25rem",
        "terminal-gutter": "1.125rem",
      },
      boxShadow: {
        terminal: "0 18px 50px rgba(0, 0, 0, 0.28)",
        "terminal-inset": "inset 0 1px 0 rgba(255, 255, 255, 0.025)",
      },
      backgroundImage: {
        "terminal-grid": "linear-gradient(rgba(56,189,248,.018) 1px, transparent 1px), linear-gradient(90deg, rgba(56,189,248,.018) 1px, transparent 1px)",
      },
    },
  },
  plugins: [],
};

export default config;
