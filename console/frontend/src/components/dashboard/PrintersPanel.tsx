import { useId, useMemo, useState } from 'react';
import { Pin, Gauge, ExternalLink, Sparkles, Loader2, Thermometer, Wind } from 'lucide-react';
import { Card } from '@spoolhard/ui/components/Card';
import { Button } from '@spoolhard/ui/components/Button';
import { StatusDot } from '@spoolhard/ui/components/StatusDot';
import {
  usePrinters,
  usePrinterAnalysis,
  useStartPrinterAnalysis,
  type AmsTray,
  type AmsUnit,
  type GcodeAnalysis,
  type Printer,
} from '../../hooks/usePrinters';
import { useSpools, type SpoolRecord } from '../../hooks/useSpools';
import { AmsSlotPicker } from './AmsSlotPicker';

export function PrintersPanel() {
  const { data: printers } = usePrinters();
  // Fetch the full spool list once per panel render so each tray card can
  // resolve its mapped spool's weight_current and K fallback without
  // firing a per-slot request. limit=200 covers every realistic setup.
  const { data: spools } = useSpools(0, 200);
  const spoolById = useMemo(() => {
    const m = new Map<string, SpoolRecord>();
    spools?.rows?.forEach((r) => m.set(r.id, r));
    return m;
  }, [spools?.rows]);

  if (!printers || printers.length === 0) {
    return (
      <Card title="Printers">
        <div className="text-sm text-text-muted">
          No printers configured. Go to Config -&gt; Setup -&gt; Bambu Lab Printers to add one.
        </div>
      </Card>
    );
  }

  return (
    <Card title={`Printers (${printers.length})`}>
      <div className="space-y-4">
        {printers.map((p) => <PrinterRow key={p.serial} p={p} spoolById={spoolById} />)}
      </div>
    </Card>
  );
}

function PrinterRow({ p, spoolById }: { p: Printer; spoolById: Map<string, SpoolRecord> }) {
  const s = p.state;
  const linkStatus: 'connected' | 'connecting' | 'disconnected' =
    s.link === 'connected' ? 'connected' : s.link === 'connecting' ? 'connecting' : 'disconnected';
  const gcode = s.gcode_state ?? 'UNKNOWN';
  const pct = typeof s.progress_pct === 'number' && s.progress_pct >= 0 ? s.progress_pct : null;
  const layerText = typeof s.layer_num === 'number' && s.layer_num >= 0
    ? `layer ${s.layer_num}${s.total_layers ? ` / ${s.total_layers}` : ''}`
    : '';
  const isPrinting = gcode === 'RUNNING' || gcode === 'PAUSE';
  const hasAnyTray = (s.ams && s.ams.length > 0) || !!s.vt_tray;
  return (
    <div className="rounded-md border border-surface-border bg-surface-input p-3 space-y-2">
      <div className="flex items-center gap-2">
        <StatusDot status={linkStatus} />
        <div className="text-sm font-medium text-text-primary">{p.name || p.serial}</div>
        <div className="text-xs text-text-muted font-mono ml-auto">{p.ip}</div>
      </div>

      {s.link === 'connected' ? (
        <>
          {/* Print status — prominent state pill, progress, layers, temps. */}
          <div className="flex items-center flex-wrap gap-2">
            <span className={`px-2 py-0.5 rounded text-[11px] font-medium uppercase tracking-wide ${gcodeStatePillClass(gcode)}`}>
              {gcode}
            </span>
            {pct !== null && (
              <span className="text-sm font-data tabular-nums text-text-data">{pct}%</span>
            )}
            {layerText && (
              <span className="text-xs text-text-muted font-mono">{layerText}</span>
            )}
            <span className="ml-auto flex items-center gap-3 text-xs font-mono text-text-muted tabular-nums">
              {typeof s.nozzle_temp === 'number' && s.nozzle_temp > 0 && (
                <span>nozzle <span className="text-text-primary">{s.nozzle_temp.toFixed(0)}°C</span></span>
              )}
              {typeof s.bed_temp === 'number' && s.bed_temp > 0 && (
                <span>bed <span className="text-text-primary">{s.bed_temp.toFixed(0)}°C</span></span>
              )}
            </span>
          </div>
          {/* Progress bar — only while actively printing; idle printers
              don't need a greyed-out bar permanently taking space. */}
          {isPrinting && pct !== null && (
            <div className="h-1.5 w-full rounded bg-surface-card overflow-hidden">
              <div
                className="h-full bg-brand-500 transition-[width] duration-500"
                style={{ width: `${pct}%` }}
              />
            </div>
          )}
          {hasAnyTray && (
            <div className="space-y-2 mt-1">
              {s.ams?.map((u) => (
                <AmsUnitBlock
                  key={u.id}
                  u={u}
                  unitNumber={u.id + 1}
                  activeTray={s.active_tray}
                  serial={p.serial}
                  spoolById={spoolById}
                />
              ))}
              {s.vt_tray && (
                <div className="grid grid-cols-2 md:grid-cols-4 gap-2">
                  <AmsSlotCard
                    t={s.vt_tray}
                    active={s.active_tray === s.vt_tray.id}
                    serial={p.serial}
                    amsUnit={254}
                    spool={s.vt_tray.spool_id ? spoolById.get(s.vt_tray.spool_id) : undefined}
                    label="External"
                    external
                  />
                </div>
              )}
            </div>
          )}
          <AnalysisBlock serial={p.serial} gcodeState={gcode} />
        </>
      ) : (
        <div className="text-xs text-text-muted">{s.error ?? s.link}</div>
      )}
    </div>
  );
}

function AmsUnitBlock({ u, unitNumber, activeTray, serial, spoolById }: {
  u: AmsUnit;
  unitNumber: number;
  activeTray: number | undefined;
  serial: string;
  spoolById: Map<string, SpoolRecord>;
}) {
  return (
    <div>
      <AmsUnitHeader u={u} unitNumber={unitNumber} />
      <div className="grid grid-cols-2 md:grid-cols-4 gap-2">
        {u.trays.map((t) => (
          <AmsSlotCard
            key={t.id}
            t={t}
            active={activeTray === t.id}
            serial={serial}
            amsUnit={u.id}
            spool={t.spool_id ? spoolById.get(t.spool_id) : undefined}
            label={`AMS ${unitNumber}·${t.id + 1}`}
          />
        ))}
      </div>
    </div>
  );
}

// Per-unit status strip: temperature + humidity (raw % when reported, else
// the 1..5 scaled drop) + drying chip when a dry cycle is active. Sits
// above the row of slot cards so the AMS block reads as one unit.
function AmsUnitHeader({ u, unitNumber }: { u: AmsUnit; unitNumber: number }) {
  const hasTemp = typeof u.temp_c === 'number' && u.temp_c > -100;
  const hasRawHum = typeof u.humidity_raw === 'number' && u.humidity_raw >= 0;
  const hasScaledHum = typeof u.humidity === 'number' && u.humidity >= 1 && u.humidity <= 5;
  if (!hasTemp && !hasRawHum && !hasScaledHum && !u.drying) return null;
  return (
    <div className="flex items-center flex-wrap gap-x-3 gap-y-1 text-xs text-text-muted mb-1.5 px-0.5">
      <span className="text-[10px] uppercase tracking-wider font-medium text-text-primary">
        AMS {unitNumber}
      </span>
      {hasTemp && (
        <span className="inline-flex items-center gap-1 font-mono tabular-nums" title="AMS chamber temperature">
          <Thermometer size={11} />
          {u.temp_c!.toFixed(1)}°C
        </span>
      )}
      {hasRawHum && (
        <span className="inline-flex items-center gap-1 font-mono tabular-nums" title={`Relative humidity (raw)${hasScaledHum ? ` · scale ${u.humidity}/5` : ''}`}>
          <HumidityDrop level={hasScaledHum ? u.humidity : 3} />
          {u.humidity_raw}% RH
        </span>
      )}
      {!hasRawHum && hasScaledHum && (
        <span className="inline-flex items-center gap-1 font-mono tabular-nums" title={`Humidity ${u.humidity}/5 (5=driest, 1=wettest)`}>
          <HumidityDrop level={u.humidity} />
          {u.humidity}/5
        </span>
      )}
      {u.drying && <DryingChip d={u.drying} />}
    </div>
  );
}

// Tear-drop SVG with a level-indexed fill. Bambu's `humidity` is 1..5 with
// **5 = driest, 1 = wettest** — matches the printer's own dot indicator.
// We mirror that meaning: level 5 → empty outline (green = good), level 1
// → fully filled (yellow = warning), intermediates interpolate hue along
// the green→yellow arc. The clipPath ID is unique per render so multiple
// AMS units on one page can't collide.
function HumidityDrop({ level }: { level: number }) {
  const id = useId();
  const safe = Math.max(1, Math.min(5, level));
  const fillFrac = (5 - safe) / 4;             // 0 at level 5 (dry), 1.0 at level 1 (wet)
  // viewBox 0..24; the drop occupies roughly y=2.5 (apex) to y=21.5 (base).
  const dropTop = 2.5;
  const dropBottom = 21.5;
  const dropHeight = dropBottom - dropTop;
  const fillY = dropBottom - dropHeight * fillFrac;
  const hue = 120 - fillFrac * 60;              // 120° green → 60° yellow
  const color = `hsl(${hue} 75% 50%)`;
  const path = 'M12 2.5c0 2.5 1.5 4.9 3.5 6.5C17 10.5 18 12.4 18 14.5a6 6 0 1 1-12 0c0-2.1 1-4 2.5-5.5C10.5 7.4 12 5 12 2.5z';
  const clipId = `drop-${id}`;
  return (
    <svg width="13" height="14" viewBox="0 0 24 24" aria-hidden>
      <defs>
        <clipPath id={clipId}><path d={path} /></clipPath>
      </defs>
      {fillFrac > 0 && (
        <rect
          x="0"
          y={fillY}
          width="24"
          height={dropHeight * fillFrac}
          fill={color}
          clipPath={`url(#${clipId})`}
        />
      )}
      <path d={path} fill="none" stroke={color} strokeWidth="1.6" strokeLinejoin="round" />
    </svg>
  );
}

function DryingChip({ d }: { d: NonNullable<AmsUnit['drying']> }) {
  const parts: string[] = [];
  if (d.filament)                       parts.push(d.filament);
  if (typeof d.temperature_c === 'number') parts.push(`${d.temperature_c}°C`);
  if (typeof d.duration_h === 'number')    parts.push(`${d.duration_h}h`);
  return (
    <span
      className="inline-flex items-center gap-1 px-1.5 py-0.5 rounded bg-amber-500/15 text-amber-300 border border-amber-500/30 text-[10px] font-mono uppercase tracking-wide"
      title="AMS drying cycle in progress"
    >
      <Wind size={10} />
      Drying{parts.length ? ` · ${parts.join(' · ')}` : ''}
    </span>
  );
}

function AmsSlotCard({ t, active, serial, amsUnit, spool, label, external }: {
  t: AmsTray;
  active: boolean;
  serial: string;
  amsUnit: number;
  spool: SpoolRecord | undefined;
  label: string;
  external?: boolean;
}) {
  const [picking, setPicking] = useState(false);
  // Colour source priority: AMS-reported tray_color wins when it's known
  // and non-zero — that's the ground truth of what's physically loaded
  // right now (RFID read, or what the user set on the printer's panel).
  // Fall back to the mapped SpoolRecord's color_code for the SpoolHard-
  // tagged case before our auto-push lands. An all-zero color is treated
  // as "unknown" rather than real black, since Bambu reports 000000FF for
  // empty / unset trays.
  const hex = (() => {
    const fromAms = t.color && t.color.length >= 6 ? t.color.slice(0, 6) : '';
    if (fromAms && fromAms !== '000000') return fromAms;
    return spool?.color_code && spool.color_code.length >= 6
      ? spool.color_code.slice(0, 6)
      : '';
  })();
  const bg  = hex ? `#${hex}` : 'transparent';
  const textOnSwatch = useMemo(() => pickReadableTextColor(hex), [hex]);

  // Show the *current estimated* weight: last weighed value minus
  // whatever live print-consumption has forecast since. The detail
  // panel keeps the breakdown ("weighed N g, − M g since last weigh")
  // for transparency, but the tile itself always reflects the running
  // estimate so users see it tick down through a print. Clamped at 0
  // so a slightly-over-budget run doesn't render as a negative number.
  const weightG = (() => {
    if (!spool || typeof spool.weight_current !== 'number' || spool.weight_current < 0) return null;
    const consumed = typeof spool.consumed_since_weight === 'number' ? spool.consumed_since_weight : 0;
    return Math.max(0, Math.round(spool.weight_current - consumed));
  })();

  // K value: prefer what the printer is actively reporting for this slot;
  // otherwise look up the stored value for this (printer, nozzle) in the
  // mapped spool's ext.k_values. The two agree most of the time, but the
  // stored value lets us display "K: 0.040" on an idle slot.
  const storedK =
    !t.k && spool?.ext?.k_values?.find((e) => e.printer === serial)?.k;
  const kVal = t.k && t.k > 0 ? t.k : (storedK || 0);

  const isEmpty = !t.material && !hex;
  const subtitle = [
    spool ? (spool.brand ? `${spool.brand}` : null) : null,
    spool?.color_name ?? null,
  ].filter(Boolean).join(' · ');

  const title = [
    t.material || 'empty',
    t.tag_uid ? `tag ${t.tag_uid}` : null,
    t.spool_override ? 'manual override' : null,
    'click to reassign',
  ].filter(Boolean).join(' · ');

  return (
    <>
      <button
        type="button"
        onClick={() => setPicking(true)}
        className={`group relative overflow-hidden rounded-md border text-left transition-all hover:border-brand-400/60 h-full flex flex-col ${active ? 'border-brand-500 ring-1 ring-brand-500/30' : 'border-surface-border'} ${isEmpty ? 'bg-surface-card/40' : 'bg-surface-card'}`}
        title={title}
      >
        {/* Color swatch — dominates the card when populated, leaving a
            legible strip for the material label at the bottom. */}
        <div
          className="h-14 w-full flex items-end px-2 py-1 relative"
          style={{
            background: isEmpty
              ? 'repeating-linear-gradient(45deg, transparent 0 6px, rgba(255,255,255,0.04) 6px 12px)'
              : bg,
          }}
        >
          <div className={`absolute top-1 left-2 text-[10px] uppercase tracking-wider font-medium ${isEmpty ? 'text-text-muted' : ''}`} style={!isEmpty ? { color: textOnSwatch, opacity: 0.85 } : undefined}>
            {label}
          </div>
          {external && !isEmpty && (
            <ExternalLink size={12} className="absolute top-1 right-2" style={{ color: textOnSwatch, opacity: 0.85 }} />
          )}
          {t.spool_override && (
            <Pin size={12} className="absolute top-1 right-2" style={!isEmpty ? { color: textOnSwatch } : { color: 'var(--color-brand-400)' }} />
          )}
          {t.remain_pct >= 0 && !isEmpty && (
            <div className="absolute bottom-1 right-2 text-[10px] font-mono" style={{ color: textOnSwatch, opacity: 0.9 }}>
              {t.remain_pct}%
            </div>
          )}
          <div
            className="text-base font-semibold font-mono"
            style={!isEmpty ? { color: textOnSwatch } : { color: 'var(--color-text-muted)' }}
          >
            {t.material || 'empty'}
          </div>
        </div>

        {/* Data strip — weight + K, truncate-friendly. Always reserves the
            subtitle line (even when the slot has no brand/colour mapped)
            so all four AMS cards line up at the same height in the grid.
            mt-auto pushes the strip to the bottom when the card is
            stretched by a taller sibling. */}
        <div className="px-2 py-1.5 text-xs space-y-0.5 mt-auto">
          <div className="text-text-muted truncate min-h-[1em]">
            {subtitle || ' '}
          </div>
          <div className="flex items-baseline gap-2 font-mono tabular-nums">
            <span className="text-brand-400">
              {weightG != null ? `${weightG} g` : <span className="text-text-muted">— g</span>}
            </span>
            {kVal > 0 && (
              <span className="text-text-muted inline-flex items-center gap-0.5 ml-auto" title={`pressure advance K${storedK ? ' (saved)' : ''}`}>
                <Gauge size={10} /> {kVal.toFixed(3)}
              </span>
            )}
          </div>
        </div>
      </button>
      {picking && (
        <AmsSlotPicker
          serial={serial}
          amsUnit={amsUnit}
          slotId={t.id}
          currentSpoolId={t.spool_id}
          isOverride={t.spool_override}
          onClose={() => setPicking(false)}
        />
      )}
    </>
  );
}

// Pick black or white text based on perceived luminance of the swatch. Keeps
// "PLA" readable over both a yellow and a navy tray without hardcoding per-
// material defaults. Falls back to text-primary when we have no hex.
// Map Bambu's gcode_state to a Tailwind pill colour so the print status is
// recognisable at a glance: green running, amber paused, blue finished,
// red failed, muted idle/preparing.
function gcodeStatePillClass(state: string): string {
  switch (state) {
    case 'RUNNING':  return 'bg-green-500/20 text-green-300 border border-green-500/30';
    case 'PAUSE':    return 'bg-amber-500/20 text-amber-300 border border-amber-500/30';
    case 'FINISH':   return 'bg-blue-500/20 text-blue-300 border border-blue-500/30';
    case 'FAILED':   return 'bg-red-500/20 text-red-300 border border-red-500/30';
    case 'PREPARE':  return 'bg-cyan-500/15 text-cyan-300 border border-cyan-500/25';
    default:         return 'bg-surface-card text-text-muted border border-surface-border';
  }
}

function pickReadableTextColor(hex: string): string {
  if (!hex || hex.length < 6) return 'var(--color-text-primary)';
  const r = parseInt(hex.slice(0, 2), 16);
  const g = parseInt(hex.slice(2, 4), 16);
  const b = parseInt(hex.slice(4, 6), 16);
  // Rec. 601 luma — good enough, avoids pulling in a colour library.
  const luma = 0.299 * r + 0.587 * g + 0.114 * b;
  return luma > 160 ? '#111827' : '#f9fafb';
}

// The analysis runs automatically when a print transitions IDLE/PREPARE
// → RUNNING (firmware fetches the 3MF over FTPS and streams the gcode
// through the on-device analyzer) AND on the first MQTT report after
// console boot when the printer is already RUNNING — so a reboot
// mid-print picks tracking back up without user action.
//
// The manual button below is the fallback: re-run when the auto-kick
// failed (network blip, transient FTPS error) or when the user wants
// to refresh the analysis after a slicer-side change. Hidden while the
// printer is idle.
function AnalysisBlock({ serial, gcodeState }: { serial: string; gcodeState?: string }) {
  const { data: analysis } = usePrinterAnalysis(serial);
  const start = useStartPrinterAnalysis();
  const printing = gcodeState === 'RUNNING' || gcodeState === 'PAUSE' || gcodeState === 'PREPARE';

  if (!analysis) return null;
  if (analysis.in_progress || start.isPending) {
    // Bytes-fetched progress (works for both stored and deflate ZIP
    // entries — for deflate we count compressed input bytes, which is
    // what we have a known total for). Indeterminate when total=0.
    const total = analysis.progress_total_bytes;
    const done  = analysis.progress_bytes;
    const pct   = total > 0 ? Math.min(100, Math.round((done / total) * 100)) : null;
    return (
      <div className="pt-2 border-t border-surface-border text-xs">
        <div className="flex items-center gap-2 text-text-muted font-mono mb-1">
          <Loader2 size={12} className="animate-spin" />
          <span>Analysing current job…</span>
          {pct !== null && (
            <span className="ml-auto tabular-nums">
              {(done / 1024).toFixed(0)} / {(total / 1024).toFixed(0)} KB · {pct}%
            </span>
          )}
          {pct === null && done > 0 && (
            <span className="ml-auto tabular-nums">{(done / 1024).toFixed(0)} KB</span>
          )}
        </div>
        <div className="h-1 bg-surface-input rounded overflow-hidden">
          <div
            className={pct === null ? 'h-full bg-brand-400/40 animate-pulse' : 'h-full bg-brand-400 transition-all'}
            style={pct === null ? { width: '40%' } : { width: `${pct}%` }}
          />
        </div>
        {analysis.running_mm > 0 && (
          <div className="mt-1 text-[10px] text-text-muted font-mono tabular-nums">
            so far: {analysis.running_grams.toFixed(1)} g · {(analysis.running_mm / 1000).toFixed(1)} m
          </div>
        )}
      </div>
    );
  }
  if (!analysis.valid) {
    // Three sub-states:
    //   - printing && no error: console booted into an active print and
    //     hasn't been asked to analyse yet — show the kick button.
    //   - error: surface it (and offer retry if printing).
    //   - idle and no error: hide (no print to analyse).
    if (!printing && !analysis.error) return null;
    return (
      <div className="pt-2 border-t border-surface-border text-xs flex items-center gap-2 flex-wrap">
        {analysis.error
          ? <span className="text-red-400 font-mono">analysis error: {analysis.error}</span>
          : <span className="text-text-muted font-mono">No analysis for the current print yet.</span>}
        {printing && (
          <Button
            variant="secondary"
            onClick={() => start.mutate({ serial })}
            disabled={start.isPending}
            className="!py-1 !px-2 ml-auto"
            title="Fetch the printer's current 3MF over FTPS and run the on-device gcode analyzer"
          >
            <Sparkles size={12} className="mr-1 inline" />
            {analysis.error ? 'Retry' : 'Analyse current print'}
          </Button>
        )}
      </div>
    );
  }
  return (
    <div className="pt-2 border-t border-surface-border">
      <AnalysisResult a={analysis} />
    </div>
  );
}

function AnalysisResult({ a }: { a: GcodeAnalysis }) {
  const active = a.gcode_state === 'RUNNING' || a.gcode_state === 'PAUSE';
  const totalConsumed = a.tools.reduce((s, t) => s + (t.grams_consumed ?? 0), 0);
  return (
    <div className="mt-1 text-xs">
      <div className="flex items-baseline gap-2 font-mono text-text-muted mb-1.5">
        <span className="text-[10px] uppercase tracking-widest">Filament</span>
        {active && (
          <span className="text-brand-400 tabular-nums">
            {totalConsumed.toFixed(1)} / {a.total_grams.toFixed(1)} g · {a.progress_pct}%
          </span>
        )}
        {!active && (
          <span className="tabular-nums">
            total {a.total_grams.toFixed(1)} g · {(a.total_mm / 1000).toFixed(1)} m
          </span>
        )}
        <span
          className="ml-auto text-[10px] uppercase tracking-widest"
          title={a.has_pct_table
            ? 'exact per-percent usage from slicer M73 hints'
            : 'no slicer M73 hints found; consumption is linearly extrapolated'}
        >
          {a.has_pct_table ? 'exact' : 'linear'}
        </span>
      </div>
      <div className="grid grid-cols-[auto,1fr,auto,auto] gap-x-3 gap-y-1 font-mono">
        {a.tools.map((t) => {
          const bg = t.color ? `#${t.color.slice(0, 6)}` : 'transparent';
          const consumed = t.grams_consumed ?? 0;
          const frac = t.grams > 0 ? Math.min(1, consumed / t.grams) : 0;
          return (
            <div key={t.tool_idx} className="contents">
              <div className="flex items-center gap-1.5 text-text-muted">
                <div className="w-2.5 h-2.5 rounded-full border border-surface-border" style={{ background: bg }} />
                T{t.tool_idx}
              </div>
              <div className="min-w-0">
                <div className="text-text-primary truncate">
                  {t.material || 'unknown'}
                  {t.ams_unit >= 0 && <span className="text-text-muted"> · AMS {t.ams_unit + 1}/{t.slot_id + 1}</span>}
                </div>
                {active && (
                  <div className="mt-0.5 h-1 w-full rounded bg-surface-input overflow-hidden">
                    <div
                      className="h-full bg-brand-500 transition-[width] duration-500"
                      style={{ width: `${(frac * 100).toFixed(1)}%` }}
                    />
                  </div>
                )}
              </div>
              <div className="text-right tabular-nums">
                {active ? (
                  <span className="text-brand-400">
                    {consumed.toFixed(1)}
                    <span className="text-text-muted"> / {t.grams.toFixed(1)} g</span>
                  </span>
                ) : (
                  <span className="text-text-primary">{t.grams.toFixed(1)} g</span>
                )}
              </div>
              <div className="text-text-muted text-right tabular-nums">{(t.mm / 1000).toFixed(1)} m</div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
