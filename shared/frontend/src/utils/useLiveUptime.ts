import { useEffect, useState } from 'react';

// Render a continuously-ticking uptime value built from a base sample
// + the time at which we received it. Between samples we extrapolate
// locally at 1 Hz so the UI doesn't sit visibly frozen for the 5-30 s
// gap between WS pushes; when a fresh sample arrives the React Query
// `dataUpdatedAt` change resets the base and the display snaps to it.
//
//   const { data, dataUpdatedAt } = useFirmwareInfo();
//   const live = useLiveUptime(data?.uptime_s, dataUpdatedAt);
//
// Returns the same `undefined` the base did until we have a real
// sample. The clock drifts at most a couple of seconds before the
// next sample lands — the firmware push side tolerates that fine.
export function useLiveUptime(
  baseS: number | undefined | null,
  baseAtMs: number | undefined,
): number | undefined {
  const [tick, setTick] = useState(() => Date.now());
  useEffect(() => {
    const id = setInterval(() => setTick(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);
  if (baseS == null || !isFinite(baseS) || baseS < 0) return undefined;
  if (!baseAtMs || baseAtMs <= 0) return baseS;
  const elapsed = Math.max(0, Math.floor((tick - baseAtMs) / 1000));
  return baseS + elapsed;
}
