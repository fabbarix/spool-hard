import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';

// One persisted crash log on the SD card. The firmware preserves the
// previous session's log under /spoolease/crashes/ whenever it boots
// after a panic / watchdog / brownout reset, indexed by `seq`.
export interface CrashEntry {
  seq:    number;
  reason: string;
  bytes:  number;
  // Unix timestamp from the SD card's last-write time. 0 when SNTP
  // wasn't ready before the crash — render a relative "—" in that case.
  mtime:  number;
}

export interface CrashListResponse {
  available: boolean;   // false when no SD card / log persistence disabled
  crashes:   CrashEntry[];
}

export function useCrashes() {
  return useQuery<CrashListResponse>({
    queryKey: ['crashes'],
    queryFn: async () => {
      const r = await fetch('/api/crashes');
      if (!r.ok) throw new Error(`/api/crashes: ${r.status}`);
      return r.json();
    },
    // The list is small and changes rarely (once per crash). The user
    // pulls the page intentionally; a 30 s background refetch is plenty.
    refetchInterval: 30_000,
    staleTime: 5_000,
  });
}

export function useDeleteCrash() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async (seq: number) => {
      const r = await fetch(`/api/crashes/${seq}`, { method: 'DELETE' });
      if (!r.ok) throw new Error(`delete crash ${seq}: ${r.status}`);
      return r.json();
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: ['crashes'] }),
  });
}

export function useDeleteAllCrashes() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: async () => {
      const r = await fetch('/api/crashes', { method: 'DELETE' });
      if (!r.ok) throw new Error(`delete crashes: ${r.status}`);
      return r.json() as Promise<{ ok: boolean; removed: number }>;
    },
    onSuccess: () => qc.invalidateQueries({ queryKey: ['crashes'] }),
  });
}

// Fetch the raw text of one crash log on demand. Not a useQuery — the
// user clicks "View" and we render in a modal; refetch on close isn't
// useful and the response can be tens of KB.
export async function fetchCrashText(seq: number): Promise<string> {
  const r = await fetch(`/api/crashes/${seq}`);
  if (!r.ok) throw new Error(`fetch crash ${seq}: ${r.status}`);
  return r.text();
}
