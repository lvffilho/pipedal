// config.ts — TONE3000 API configuration
// T3K_API points to production. VITE_T3K_API_DOMAIN can override for development.
// Trailing slashes are stripped so `${T3K_API}/api/...` never produces a double slash —
// Vercel 308-redirects double-slash paths, and redirects drop CORS headers.
export const T3K_API = (
  'https://www.tone3000.com'
).replace(/\/+$/, '');

// The TONE3000 OAuth client id: a publishable key (t3k_pub_...), safe to ship. The CMake build sets
// VITE_T3K_PUBLISHABLE_KEY from PIPEDAL_T3K_PUBLISHABLE_KEY, so the web UI and pipedald agree; the
// fallback (the CMake default) serves a plain `npm run build` / `npm run dev`. Never a secret key.
export const PUBLISHABLE_KEY: string =
  import.meta.env.VITE_T3K_PUBLISHABLE_KEY || "t3k_pub_47aYRwEal7tgm_i62bBlQP45wcr_7iwl";

// Per-demo keys — fall back to the shared PUBLISHABLE_KEY for local dev
export const PUBLISHABLE_KEY_SELECT = PUBLISHABLE_KEY;
export const PUBLISHABLE_KEY_LOAD = PUBLISHABLE_KEY;
export const PUBLISHABLE_KEY_FULL = PUBLISHABLE_KEY;

