// Render a seconds-since-boot value as a compact human string.
// Examples:
//   42        → "42s"
//   125       → "2m 5s"
//   7320      → "2h 2m"
//   90061     → "1d 1h"
//   undefined → "—"
//
// Designed for inline status lines, not log output — drops the smaller
// unit once the larger one is hours+ so the result stays under 8 chars.
export function formatUptime(uptimeS: number | undefined | null): string {
  if (uptimeS == null || !isFinite(uptimeS) || uptimeS < 0) return '—';
  const s = Math.floor(uptimeS);
  if (s < 60)    return `${s}s`;
  const m = Math.floor(s / 60);
  if (m < 60)    return `${m}m ${s % 60}s`;
  const h = Math.floor(m / 60);
  if (h < 24)    return `${h}h ${m % 60}m`;
  const d = Math.floor(h / 24);
  return `${d}d ${h % 24}h`;
}
