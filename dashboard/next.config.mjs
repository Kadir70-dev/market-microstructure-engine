/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // better-sqlite3 is a native module — keep it external to the server bundle.
  // It is lazy-required only when DASHBOARD_DB_PATH points at a real DB, so the
  // app still runs in DEMO mode even if the native build is unavailable.
  experimental: {
    serverComponentsExternalPackages: ["better-sqlite3"],
  },
};

export default nextConfig;
