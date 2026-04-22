import { useState, useEffect, useCallback, useRef } from 'react';
import type { OtaInProgressT, ScaleLinkState } from './useOtaConfig';

// Per-product OTA-update state machine driven by /api/ota-status polling.
//
// The lifecycle of a click → "✓ Updated" cycle is:
//   1. trigger(expectedVersion, originalVersion) sets `phase = 'inflight'`
//      and stamps Date.now() so we can time-box the run.
//   2. While inflight, every status poll feeds `observe()`, which derives
//      the user-visible message (Starting / Updating XX% / Rebooting).
//   3. Two terminal states:
//        - success when the device's reported `firmware_current` flips to
//          `expectedVersion` (and is no longer the original)
//        - failed when 8 minutes elapse without success — generous because
//          a manifest fetch + dual flash + reboot can run several minutes
//          on a flaky network.
//   4. Success holds for 8 s so the user sees the confirmation, then
//      auto-resets to idle.
//
// All the per-product specifics (which status block to read, link-state
// awareness for the scale) come in via `observe()` args — keeps the hook
// shape identical for both console + scale callers.

export type UpdaterPhase = 'idle' | 'inflight' | 'success' | 'failed';

export interface UpdaterDerived {
  phase: UpdaterPhase;
  // Human-readable substate while inflight ("Starting…", "Updating
  // firmware: 42%", "Rebooting…"). Empty when phase is idle/success/failed.
  message: string;
  // 0..100 progress for the bar; -1 when no concrete percent (e.g.
  // pre-first-tick or rebooting).
  percent: number;
  // Populated on terminal states.
  successVersion?: string;
  failureReason?: string;
}

export interface UpdaterObservation {
  in_progress?: OtaInProgressT;
  current_version?: string;       // firmware_current from status
  link?: ScaleLinkState;          // only for the scale
  poll_failed: boolean;           // true when react-query reports an error this tick
  poll_age_ms: number;            // ms since the last successful status response
}

const FAILURE_TIMEOUT_MS = 8 * 60 * 1000;
const SUCCESS_HOLD_MS    = 8 * 1000;
const REBOOTING_AFTER_MS = 6 * 1000;   // no progress within this window → "rebooting"

interface InflightState {
  startedAt: number;
  expectedVersion: string;
  originalVersion: string;
}

export function useOtaUpdater() {
  const [phase, setPhase] = useState<UpdaterPhase>('idle');
  const [message, setMessage] = useState('');
  const [percent, setPercent] = useState(-1);
  const [successVersion, setSuccessVersion] = useState<string | undefined>(undefined);
  const [failureReason, setFailureReason] = useState<string | undefined>(undefined);

  // Refs so the observe() callback doesn't re-bind on every render. The
  // setters above are stable so they're fine to call from inside.
  const inflight = useRef<InflightState | null>(null);
  const successAt = useRef<number | null>(null);

  const trigger = useCallback((expectedVersion: string, originalVersion: string) => {
    inflight.current = {
      startedAt: Date.now(),
      expectedVersion,
      originalVersion,
    };
    successAt.current = null;
    setPhase('inflight');
    setMessage('Starting…');
    setPercent(-1);
    setSuccessVersion(undefined);
    setFailureReason(undefined);
  }, []);

  const reset = useCallback(() => {
    inflight.current = null;
    successAt.current = null;
    setPhase('idle');
    setMessage('');
    setPercent(-1);
    setSuccessVersion(undefined);
    setFailureReason(undefined);
  }, []);

  const observe = useCallback((o: UpdaterObservation) => {
    const ifs = inflight.current;
    if (!ifs) return;

    // Terminal: success — current version flipped to what we expected.
    // Only valid once we've actually heard a fresh poll back; ignore mid-
    // reboot stale data by checking poll_age_ms.
    if (
      o.current_version &&
      ifs.expectedVersion &&
      o.current_version === ifs.expectedVersion &&
      o.current_version !== ifs.originalVersion &&
      o.poll_age_ms < 4000
    ) {
      if (successAt.current === null) successAt.current = Date.now();
      setPhase('success');
      setMessage('');
      setPercent(100);
      setSuccessVersion(o.current_version);
      return;
    }

    // Terminal: timeout.
    const elapsed = Date.now() - ifs.startedAt;
    if (elapsed > FAILURE_TIMEOUT_MS) {
      setPhase('failed');
      setMessage('');
      setPercent(-1);
      setFailureReason('Update timed out — device did not come back online');
      inflight.current = null;
      return;
    }

    // Inflight progression. Priority of cues:
    //   1. concrete in_progress.percent → "Updating <kind>: X%"
    //   2. for scale: link === 'offline' or 'waiting' → "Rebooting…"
    //   3. for console: poll_failed (server unreachable) → "Rebooting…"
    //   4. >REBOOTING_AFTER_MS since click with no concrete progress → "Rebooting…"
    //   5. else → "Starting…"
    if (o.in_progress && o.in_progress.percent >= 0) {
      const kindLabel = o.in_progress.kind ? `Updating ${o.in_progress.kind}` : 'Updating';
      setMessage(`${kindLabel}: ${o.in_progress.percent}%`);
      setPercent(o.in_progress.percent);
      return;
    }
    const looksRebooting =
      (o.link !== undefined && (o.link === 'offline' || o.link === 'waiting')) ||
      o.poll_failed ||
      elapsed > REBOOTING_AFTER_MS;
    if (looksRebooting) {
      setMessage('Rebooting…');
      setPercent(-1);
    } else {
      setMessage('Starting…');
      setPercent(-1);
    }
  }, []);

  // Auto-dismiss the success banner after the hold window so the button
  // returns to "Update now" without forcing the user to click anything.
  useEffect(() => {
    if (phase !== 'success' || successAt.current === null) return;
    const remaining = SUCCESS_HOLD_MS - (Date.now() - successAt.current);
    if (remaining <= 0) {
      reset();
      return;
    }
    const t = setTimeout(reset, remaining);
    return () => clearTimeout(t);
  }, [phase, message, reset]);

  const derived: UpdaterDerived = {
    phase,
    message,
    percent,
    successVersion,
    failureReason,
  };

  return { ...derived, trigger, observe, reset };
}
