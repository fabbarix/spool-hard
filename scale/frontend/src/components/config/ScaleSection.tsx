import { useState, useEffect, useMemo } from 'react';
import { Scale, RotateCcw, Plus, Trash2, Check, AlertTriangle, Crosshair, Activity } from 'lucide-react';
import {
  useScaleConfig, useSetScaleConfig, useTare,
  useAddCalPoint, useClearCal, useCaptureRaw,
} from '../../hooks/useScaleConfig';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { InputField } from '@spoolhard/ui/components/InputField';
import { Button } from '@spoolhard/ui/components/Button';
import type { CalPoint } from '../../types/api';

// Least-squares slope through the origin (tare is our zero, so the fitted
// line MUST pass through (Δ=0, w=0) — we only solve for the slope).
//   w ≈ a · Δ            with a = Σ(Δ·w) / Σ(Δ²)
// Returns per-point residuals in grams plus summary stats so the UI can
// flag outliers. Piecewise-linear interp through the same points would
// give zero residual at every point by construction, so it's no help for
// a linearity check — we compare against a single best-fit line instead.
interface LinearityFit {
  slope:            number;  // grams per ADC-count delta
  residuals_g:      number[];  // signed: actual_weight − fitted_weight
  maxAbsResidual_g: number;
  rms_g:            number;
  r2:               number;   // coefficient of determination
}
function fitLinearThroughOrigin(points: CalPoint[]): LinearityFit | null {
  if (points.length < 2) return null;
  let sxx = 0, sxy = 0;
  for (const p of points) {
    sxx += p.delta * p.delta;
    sxy += p.delta * p.weight_g;
  }
  if (sxx === 0) return null;
  const slope = sxy / sxx;

  const residuals_g = points.map((p) => p.weight_g - slope * p.delta);
  const maxAbsResidual_g = residuals_g.reduce((m, r) => Math.max(m, Math.abs(r)), 0);
  const rms_g = Math.sqrt(residuals_g.reduce((s, r) => s + r * r, 0) / residuals_g.length);

  // R² around the origin model: 1 − SS_res / SS_tot (SS_tot = Σw²).
  const ssRes = residuals_g.reduce((s, r) => s + r * r, 0);
  const ssTot = points.reduce((s, p) => s + p.weight_g * p.weight_g, 0);
  const r2 = ssTot > 0 ? Math.max(0, 1 - ssRes / ssTot) : 0;

  return { slope, residuals_g, maxAbsResidual_g, rms_g, r2 };
}

// Threshold for flagging a single point as an outlier: max(0.5 g, 1% of the
// point's weight). Below that, real ADC noise and the HX711's ~0.05% spec
// make residuals meaningless noise; above it, the point is probably wrong
// (mis-keyed weight, spool still on the pan, etc.).
function isOutlier(residual_g: number, weight_g: number): boolean {
  const tol = Math.max(0.5, weight_g * 0.01);
  return Math.abs(residual_g) > tol;
}

function CalPointRow({ point, index, residual_g }: {
  point: CalPoint;
  index: number;
  residual_g?: number;
}) {
  const outlier = residual_g !== undefined && isOutlier(residual_g, point.weight_g);
  return (
    <div className="flex items-center gap-3 py-2 border-b border-surface-border last:border-0">
      <span className="w-6 text-center text-xs text-text-muted font-mono">{index + 1}</span>
      <span className="flex-1 font-mono text-sm text-text-data">{point.weight_g} g</span>
      {residual_g !== undefined && (
        <span className={`font-mono text-[11px] tabular-nums ${outlier ? 'text-status-warn' : 'text-text-muted'}`}
              title="Residual vs. best-fit line. Large values = probable measurement error.">
          {outlier && <AlertTriangle size={11} className="inline mr-0.5" />}
          {residual_g >= 0 ? '+' : ''}{residual_g.toFixed(2)} g
        </span>
      )}
      <span className="font-mono text-xs text-text-muted">Δ {point.delta >= 0 ? '+' : ''}{point.delta.toLocaleString()}</span>
    </div>
  );
}

export function ScaleSection() {
  const { data } = useScaleConfig();
  const setConfig = useSetScaleConfig();
  const tare = useTare();
  const addPoint = useAddCalPoint();
  const clearCal = useClearCal();
  const captureRaw = useCaptureRaw();

  const [samples, setSamples] = useState('10');
  const [stableThreshold, setStableThreshold] = useState('1.0');
  const [stableCount, setStableCount] = useState('5');
  const [loadDetect, setLoadDetect] = useState('2.0');
  const [precision, setPrecision] = useState('1');
  const [rounding, setRounding] = useState<'round' | 'truncate'>('round');
  const [newWeight, setNewWeight] = useState('');
  const [confirmClear, setConfirmClear] = useState(false);

  useEffect(() => {
    if (data) {
      setSamples(String(data.samples));
      setStableThreshold(String(data.stable_threshold));
      setStableCount(String(data.stable_count));
      setLoadDetect(String(data.load_detect));
      setPrecision(String(data.precision));
      setRounding(data.rounding);
    }
  }, [data]);

  const points = data?.points || [];
  const isBusy = tare.isPending || addPoint.isPending || clearCal.isPending;
  // Best-fit line through origin; null when there aren't enough points.
  // Used to surface how linear the captured points actually are — the
  // piecewise-linear model the firmware uses would show zero residual at
  // every point and tell us nothing.
  const fit = useMemo(() => fitLinearThroughOrigin(points), [points]);
  const hasOutlier = fit?.residuals_g.some((r, i) => isOutlier(r, points[i].weight_g)) ?? false;

  return (
    <div className="space-y-4">
      {/* ── Calibration ───────────────────────────────────── */}
      <SectionCard title="Calibration" icon={<Crosshair size={16} />}>
        {/* Status */}
        <div className="flex items-center justify-between">
          <span className={`flex items-center gap-1.5 text-xs font-mono ${data?.calibrated ? 'text-status-connected' : 'text-status-error'}`}>
            {data?.calibrated
              ? <><Check size={12} /> {data.num_points} point{data.num_points !== 1 ? 's' : ''}</>
              : <><AlertTriangle size={12} /> Not calibrated</>}
          </span>
        </div>

        {/* Step 1: Tare */}
        <div className="rounded-card border border-surface-border bg-surface-input p-3 space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-xs font-semibold uppercase tracking-wider text-text-secondary">Step 1 — Tare</span>
            {data?.tare_raw !== 0 && <span className="text-[10px] text-status-connected">Done</span>}
          </div>

          {/* Current tare — shown as the hero of this card so the user can
              see the freshly captured zero the moment Set Zero resolves.
              useTare patches the cache in onSuccess, so this updates
              synchronously with the button's finished state. */}
          <div className="flex items-baseline gap-2">
            <span className="text-xs text-text-muted">Current zero:</span>
            <span className="font-mono text-lg text-text-data tabular-nums">
              {data?.tare_raw ? data.tare_raw.toLocaleString() : '—'}
            </span>
            <span className="text-[10px] text-text-muted">ADC counts</span>
          </div>

          <p className="text-[11px] text-text-muted">Remove all weight from the scale, then tap Set Zero.</p>
          <Button variant="secondary" onClick={() => tare.mutate()} disabled={isBusy}>
            <RotateCcw size={14} className={`mr-1.5 inline ${tare.isPending ? 'animate-spin' : ''}`} />
            {tare.isPending ? 'Taring...' : 'Set Zero'}
          </Button>
        </div>

        {/* Step 2: Add calibration points */}
        <div className="rounded-card border border-surface-border bg-surface-input p-3 space-y-2">
          <span className="text-xs font-semibold uppercase tracking-wider text-text-secondary">Step 2 — Add Reference Weights</span>
          <p className="text-[11px] text-text-muted">
            Place a known weight on the scale, enter its value, and press Add Point.
            Add 2–6 points across your expected range for best accuracy.
          </p>

          {/* Current points table */}
          {points.length > 0 && (
            <div className="rounded border border-surface-border bg-surface-body p-2">
              {points.map((p, i) => (
                <CalPointRow
                  key={i}
                  point={p}
                  index={i}
                  residual_g={fit?.residuals_g[i]}
                />
              ))}
            </div>
          )}

          {/* Linearity summary — appears once we have 2+ points. The
              firmware uses piecewise-linear interp (zero residual at every
              point), so this compares the captured points against a single
              best-fit line through origin to spot:
                - A mis-entered weight (one row wildly off)
                - A drifting tare captured between points
                - A sticky spool / surface mid-capture
              An outlier threshold of max(0.5 g, 1 %·weight) ignores normal
              ADC noise but catches real mistakes. */}
          {fit && (
            <div className={`rounded border p-2 text-xs space-y-1 ${
              hasOutlier
                ? 'border-status-warn/50 bg-status-warn/5'
                : 'border-surface-border bg-surface-body'
            }`}>
              <div className="flex items-center justify-between">
                <span className="flex items-center gap-1.5 font-semibold text-text-secondary uppercase tracking-wider text-[11px]">
                  <Activity size={12} />
                  Linearity check
                </span>
                <span className={`font-mono tabular-nums ${
                  hasOutlier ? 'text-status-warn' : 'text-status-connected'
                }`}>
                  {hasOutlier ? 'outlier flagged' : 'looks linear'}
                </span>
              </div>
              <div className="grid grid-cols-3 gap-2 font-mono text-[11px] text-text-muted">
                <div>
                  <div className="text-[10px] uppercase tracking-wider">max residual</div>
                  <div className={`text-sm tabular-nums ${hasOutlier ? 'text-status-warn' : 'text-text-primary'}`}>
                    ±{fit.maxAbsResidual_g.toFixed(2)} g
                  </div>
                </div>
                <div>
                  <div className="text-[10px] uppercase tracking-wider">rms</div>
                  <div className="text-sm tabular-nums text-text-primary">
                    {fit.rms_g.toFixed(2)} g
                  </div>
                </div>
                <div>
                  <div className="text-[10px] uppercase tracking-wider">R²</div>
                  <div className="text-sm tabular-nums text-text-primary">
                    {fit.r2 >= 0.999995 ? '1.0000' : fit.r2.toFixed(5)}
                  </div>
                </div>
              </div>
              {hasOutlier && (
                <div className="text-[11px] text-status-warn leading-relaxed pt-1">
                  One or more points are more than max(0.5&nbsp;g, 1&nbsp;%) off the
                  best-fit line. Re-capture the flagged row — double-check the
                  reference weight, that the scale was stable, and that nothing
                  else was touching the pan.
                </div>
              )}
            </div>
          )}

          {/* Add new point */}
          <div className="flex items-end gap-2">
            <div className="w-32">
              <InputField
                label="Weight (g)"
                value={newWeight}
                onChange={(e) => setNewWeight(e.target.value)}
                type="number"
                placeholder="e.g. 200"
              />
            </div>
            <Button
              onClick={() => {
                const w = parseFloat(newWeight);
                if (w > 0) {
                  addPoint.mutate(w);
                  setNewWeight('');
                }
              }}
              disabled={isBusy || !newWeight || parseFloat(newWeight) <= 0 || points.length >= 8}
            >
              <Plus size={14} className="mr-1 inline" />
              {addPoint.isPending ? 'Capturing...' : 'Add Point'}
            </Button>

            {/* Read current raw (helper) */}
            <Button
              variant="secondary"
              onClick={() => captureRaw.mutate()}
              disabled={captureRaw.isPending}
            >
              {captureRaw.isPending ? '...' : captureRaw.data ? `Raw: ${captureRaw.data.raw.toLocaleString()}` : 'Read Raw'}
            </Button>
          </div>

          {addPoint.isSuccess && (
            <div className="text-xs text-status-connected">Point added.</div>
          )}

          {points.length >= 8 && (
            <div className="text-xs text-status-info">Maximum 8 calibration points reached.</div>
          )}
        </div>

        {/* Clear calibration */}
        {points.length > 0 && (
          <div className="flex items-center gap-2">
            {confirmClear ? (
              <>
                <span className="text-xs text-status-error">Delete all calibration data?</span>
                <Button variant="danger" onClick={() => { clearCal.mutate(); setConfirmClear(false); }} disabled={isBusy}>
                  Yes, clear
                </Button>
                <Button variant="secondary" onClick={() => setConfirmClear(false)}>
                  Cancel
                </Button>
              </>
            ) : (
              <Button variant="danger" onClick={() => setConfirmClear(true)}>
                <Trash2 size={14} className="mr-1 inline" />
                Clear Calibration
              </Button>
            )}
          </div>
        )}
      </SectionCard>

      {/* ── Display ───────────────────────────────────────── */}
      <SectionCard title="Display" icon={<Scale size={16} />}>
        <div className="grid grid-cols-2 gap-3">
          <div>
            <label className="block text-sm">
              <span className="mb-1 block text-sm text-text-secondary">Decimal places</span>
              <select
                value={precision}
                onChange={(e) => setPrecision(e.target.value)}
                className="w-full bg-surface-input border border-surface-border rounded-button px-3 py-2 text-sm text-text-primary focus:outline-none focus:border-brand-500/50 focus:ring-1 focus:ring-brand-500/20 transition-colors"
              >
                {[0, 1, 2, 3, 4].map((n) => (
                  <option key={n} value={n}>{n} — {n === 0 ? '1 g' : `0.${'0'.repeat(n - 1)}1 g`}</option>
                ))}
              </select>
            </label>
          </div>
          <div>
            <label className="block text-sm">
              <span className="mb-1 block text-sm text-text-secondary">Rounding</span>
              <select
                value={rounding}
                onChange={(e) => setRounding(e.target.value as 'round' | 'truncate')}
                className="w-full bg-surface-input border border-surface-border rounded-button px-3 py-2 text-sm text-text-primary focus:outline-none focus:border-brand-500/50 focus:ring-1 focus:ring-brand-500/20 transition-colors"
              >
                <option value="round">Round (nearest)</option>
                <option value="truncate">Truncate (floor)</option>
              </select>
            </label>
          </div>
        </div>
      </SectionCard>

      {/* ── Sampling ──────────────────────────────────────── */}
      <SectionCard title="Sampling" icon={<Scale size={16} />}>
        <div className="grid grid-cols-2 gap-3">
          <InputField label="Weight samples" value={samples} onChange={(e) => setSamples(e.target.value)} type="number" />
          <InputField label="Stable threshold (g)" value={stableThreshold} onChange={(e) => setStableThreshold(e.target.value)} type="number" />
          <InputField label="Stable count required" value={stableCount} onChange={(e) => setStableCount(e.target.value)} type="number" />
          <InputField label="Load detect (g)" value={loadDetect} onChange={(e) => setLoadDetect(e.target.value)} type="number" />
        </div>
        <p className="text-[11px] text-text-muted leading-relaxed">
          <strong>Samples</strong> — readings averaged per cycle.
          <strong> Threshold</strong> — max drift (g) to count as stable.
          <strong> Count</strong> — consecutive stable readings to confirm.
          <strong> Detect</strong> — min weight to register a load.
        </p>
      </SectionCard>

      {/* Save display + sampling */}
      <div className="flex items-center gap-3">
        <Button
          onClick={() => setConfig.mutate({
            samples: parseInt(samples) || 10,
            stable_threshold: parseFloat(stableThreshold) || 1.0,
            stable_count: parseInt(stableCount) || 5,
            load_detect: parseFloat(loadDetect) || 2.0,
            // `|| 1` would coerce a valid 0 to 1 — precision=0 is a legitimate
            // choice (integer grams only) so use a NaN-aware fallback instead.
            precision: Number.isFinite(parseInt(precision)) ? parseInt(precision) : 1,
            rounding,
          })}
          disabled={setConfig.isPending}
        >
          {setConfig.isPending ? 'Saving...' : 'Save Display & Sampling'}
        </Button>
        {setConfig.isSuccess && (
          <span className="text-xs text-status-connected">Saved. Restart to apply sampling changes.</span>
        )}
      </div>
    </div>
  );
}
