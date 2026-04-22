import { useState, useEffect, useCallback, useRef } from 'react';
import type { OtaInProgressT } from '../types/api';

// Per-product OTA-update state machine driven by /api/ota-status polling.
//
// The lifecycle of a click → "✓ Updated" cycle is:
//   1. trigger(expectedVersion, originalVersion) sets `phase = 'inflight'`
//      and stamps Date.now() so we can time-box the run.
//   2. While inflight, every status poll feeds `observe()`, which derives
//      the user-visible message (Starting / Updating XX% / Rebooting).
//   3. Two terminal states:
//        - success when `firmware_current` flips to `expectedVersion`
//        - failed when 8 minutes elapse without success
//   4. Success holds for 8 s so the user sees the confirmation, then
//      auto-resets to idle.

export type UpdaterPhase = 'idle' | 'inflight' | 'success' | 'failed';

export interface UpdaterDerived {
  phase: UpdaterPhase;
  message: string;
  percent: number;
  successVersion?: string;
  failureReason?: string;
}

export interface UpdaterObservation {
  in_progress?: OtaInProgressT;
  current_version?: string;
  poll_failed: boolean;
  poll_age_ms: number;
}

const FAILURE_TIMEOUT_MS = 8 * 60 * 1000;
const SUCCESS_HOLD_MS    = 8 * 1000;
const REBOOTING_AFTER_MS = 6 * 1000;

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

  const inflight = useRef<InflightState | null>(null);
  const successAt = useRef<number | null>(null);

  const trigger = useCallback((expectedVersion: string, originalVersion: string) => {
    inflight.current = { startedAt: Date.now(), expectedVersion, originalVersion };
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

    const elapsed = Date.now() - ifs.startedAt;
    if (elapsed > FAILURE_TIMEOUT_MS) {
      setPhase('failed');
      setMessage('');
      setPercent(-1);
      setFailureReason('Update timed out — device did not come back online');
      inflight.current = null;
      return;
    }

    if (o.in_progress && o.in_progress.percent >= 0) {
      const kindLabel = o.in_progress.kind ? `Updating ${o.in_progress.kind}` : 'Updating';
      setMessage(`${kindLabel}: ${o.in_progress.percent}%`);
      setPercent(o.in_progress.percent);
      return;
    }
    const looksRebooting = o.poll_failed || elapsed > REBOOTING_AFTER_MS;
    if (looksRebooting) {
      setMessage('Rebooting…');
      setPercent(-1);
    } else {
      setMessage('Starting…');
      setPercent(-1);
    }
  }, []);

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

  return {
    phase, message, percent, successVersion, failureReason,
    trigger, observe, reset,
  };
}
