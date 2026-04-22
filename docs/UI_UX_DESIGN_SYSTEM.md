# SpoolHard Design System

A dark, instrument-panel UI designed for embedded IoT scale devices. Built with React, TailwindCSS v4, JetBrains Mono, DM Sans, and Lucide icons. This document defines the visual language shared across SpoolHard products (Scale firmware, Console web interface).

---

## Design Philosophy

**"Precision Instrument"** — dark industrial surfaces, amber data readouts, clean bezels. Every element earns its pixels. The aesthetic draws from lab instruments and terminal UIs: data-dense, purposeful, immediately legible in any lighting.

**Principles:**
- **Dark-first.** All surfaces are dark charcoal/slate. Light themes are not supported.
- **Amber means data.** Weight values, tag IDs, active selections — anything the user came here to read — is amber (#f0b429).
- **Teal means OK.** Connected, calibrated, done — teal (#2dd4bf) signals health.
- **Red means stop.** Errors, disconnections, destructive actions — red (#f87171) demands attention.
- **Monospace for values.** Anything that's a number, ID, version, or raw data uses JetBrains Mono. Everything else uses DM Sans.
- **Motion is earned.** Entrance animations (staggered fadeInUp) on page load. Pulse on connecting states. No gratuitous animation elsewhere.

---

## Color Palette

### Brand
| Token | Hex | Usage |
|-------|-----|-------|
| `brand-400` | `#fbbf24` | Button hover, lighter accent |
| `brand-500` | `#f0b429` | Primary buttons, data values, active tabs, badge text |
| `brand-600` | `#d69e2e` | Pressed/active states |

### Surfaces
| Token | Hex | Usage |
|-------|-----|-------|
| `surface-body` | `#0f1117` | Page background |
| `surface-header` | `#0a0c10` | Top header bar |
| `surface-card` | `#1a1d27` | Cards, panels, sections |
| `surface-card-hover` | `#1e2231` | Card/button hover states |
| `surface-border` | `#252830` | Card borders, dividers, scrollbar thumb |
| `surface-input` | `#12141c` | Input field backgrounds |

### Status
| Token | Hex | Usage |
|-------|-----|-------|
| `status-connected` | `#2dd4bf` | WiFi connected, calibrated, success states |
| `status-connecting` | `#fbbf24` | WiFi connecting, pending (with pulse animation) |
| `status-disconnected` | `#f87171` | WiFi disconnected, errors, uncalibrated |
| `status-ok` | `#2dd4bf` | Alias for connected |
| `status-error` | `#f87171` | Alias for disconnected |
| `status-info` | `#64748b` | Neutral informational states |

### Text
| Token | Hex | Usage |
|-------|-----|-------|
| `text-primary` | `#e2e8f0` | Main body text, headings |
| `text-secondary` | `#64748b` | Labels, descriptions, inactive tabs |
| `text-muted` | `#475569` | Hints, disabled text, timestamps in logs |
| `text-data` | `#f0b429` | Weight values, numeric readouts (= brand-500) |

### Log Entry Colors
| Token | Hex | Usage |
|-------|-----|-------|
| `log-event` | `#2dd4bf` | Weight/NFC events (teal) |
| `log-in` | `#38bdf8` | Inbound console messages (sky blue) |
| `log-out` | `#f0b429` | Outbound console messages (amber) |
| `log-sys` | `#475569` | System/connection events (slate) |
| `log-err` | `#f87171` | Errors (red) |

---

## Typography

| Role | Font | Size | Weight | Tracking | Example |
|------|------|------|--------|----------|---------|
| **Page heading** | DM Sans | base (16px) | 600 (semibold) | normal | Device name in header |
| **Section title** | DM Sans | sm (14px) | 600 (semibold) | wider (0.05em) | "DEVICE NAME", "WIFI" |
| **Group label** | DM Sans | 11px | 700 (bold) | 0.15em | "── SETUP ──" dividers |
| **Body text** | DM Sans | sm (14px) | 400 (normal) | normal | Descriptions, form labels |
| **Helper text** | DM Sans | 11px | 400 (normal) | normal | Inline instructions |
| **Data value** | JetBrains Mono | 2xl (24px) | 600 (semibold) | tabular-nums | "0.0 g", "1234" |
| **Hero value** | JetBrains Mono | 4xl (36px) | 600 (semibold) | tabular-nums | Weight on dashboard |
| **Badge/tag** | JetBrains Mono | xs (12px) | 400 (normal) | normal | "B019142F" |
| **Version** | JetBrains Mono | xs (12px) | 400 (normal) | normal | "v0.6.2-cpp-2" |
| **Log entry** | JetBrains Mono | xs (12px) | 400 (normal) | normal | Timestamp + message |
| **Button** | DM Sans | sm (14px) | 500-600 | normal | "Save", "Flash to device" |
| **Tab** | DM Sans | sm (14px) | 500 (active) / 400 (inactive) | normal | "Dashboard", "Configuration" |
| **Stat label** | DM Sans | 10px | 500 | widest (0.1em) | "WEIGHT", "RAW" |

**Rules:**
- `tabular-nums` on all numeric displays for column alignment.
- `uppercase tracking-wider` on all section titles and card headers.
- `uppercase tracking-widest` on stat card labels (10px).
- Never use serif fonts.

**Google Fonts import:**
```html
<link href="https://fonts.googleapis.com/css2?family=DM+Sans:ital,opsz,wght@0,9..40,300;0,9..40,400;0,9..40,500;0,9..40,600;0,9..40,700;1,9..40,400&family=JetBrains+Mono:wght@400;500;600;700&display=swap" rel="stylesheet">
```

---

## Border Radius

| Token | Value | Usage |
|-------|-------|-------|
| `radius-card` | 10px | Cards, panels, sections, drop zones |
| `radius-badge` | 6px | Tag badges, small pills |
| `radius-button` | 6px | Buttons, inputs, selects |

---

## Animation

### Entrance: `fadeInUp`
```css
@keyframes fadeInUp {
  from { opacity: 0; transform: translateY(12px); }
  to   { opacity: 1; transform: translateY(0); }
}
.animate-in { animation: fadeInUp 0.4s ease-out both; }
```
Applied with staggered `animation-delay` (0, 50, 100, 150, 200ms) to dashboard cards and config sections.

### Status: `pulse-glow`
```css
@keyframes pulse-glow {
  0%, 100% { opacity: 1; }
  50%      { opacity: 0.4; }
}
.animate-pulse-glow { animation: pulse-glow 1.5s ease-in-out infinite; }
```
Used on the "connecting" StatusDot.

### Transitions
All interactive elements use `transition-all duration-200` for smooth hover/focus changes.

---

## Components

### StatusDot
A 10px colored circle indicating connection/status state.

| State | Color | Effect |
|-------|-------|--------|
| `connected` | Teal | `shadow-[0_0_6px_rgba(45,212,191,0.5)]` glow |
| `connecting` | Amber | `pulse-glow` animation |
| `disconnected` | Red | No effect |

### Button
Three variants, all with `rounded-button px-4 py-2 text-sm font-medium transition-all duration-200 disabled:opacity-50`:

| Variant | Background | Text | Border |
|---------|-----------|------|--------|
| `primary` | `brand-500` → hover `brand-400` | `surface-body` (dark) | none |
| `secondary` | `surface-card` → hover `surface-card-hover` | `text-primary` | `surface-border` |
| `danger` | `red-500/10` → hover `red-500/20` | `status-error` | `red-500/30` |

Icons (Lucide, size 14) go before the label with `mr-1.5 inline`.

### Card
Container for log panels. Has optional colored left accent border (2px).

| Part | Style |
|------|-------|
| Container | `rounded-card border border-surface-border bg-surface-card overflow-hidden` |
| Header | `flex items-center justify-between border-b border-surface-border px-4 py-3` |
| Title | `text-xs font-semibold uppercase tracking-wider text-text-secondary` |
| Body | `p-4` |

Accent colors in use: teal (`#2dd4bf`) for Event Log, amber (`#f0b429`) for Console Log.

### SectionCard
Container for config form sections. Similar to Card but with padding, icon, optional description, and highlight mode.

| Part | Style |
|------|-------|
| Container | `rounded-card border border-surface-border bg-surface-card p-5 space-y-4` |
| Highlighted | `border-brand-500/50 shadow-[0_0_15px_rgba(240,180,41,0.05)]` |
| Title row | `flex items-center gap-2` |
| Icon | `text-text-muted`, Lucide size 16 |
| Title | `text-sm font-semibold uppercase tracking-wider text-text-secondary` |
| Description | `mt-1 text-xs text-text-muted leading-relaxed` |

### Badge
Inline pill for tag IDs and counts.

| Style | Value |
|-------|-------|
| Background | `brand-500/10` → hover `brand-500/20` |
| Text | `brand-400`, JetBrains Mono, xs |
| Border | `brand-500/20` → hover `brand-500/40` |
| Radius | `radius-badge` (6px) |
| Padding | `px-2.5 py-1` |

### InputField
Dark-themed text input with label.

| Part | Style |
|------|-------|
| Label | `text-sm text-text-secondary` |
| Input | `bg-surface-input border-surface-border rounded-button px-3 py-2 text-sm text-text-primary` |
| Placeholder | `text-text-muted` |
| Focus | `border-brand-500/50 ring-1 ring-brand-500/20` |

### PasswordField
InputField with a show/hide toggle button (Eye/EyeOff icons from Lucide) positioned `absolute right-2`.

### DropZone
Multi-state file drop target with validation. See **Component States** section.

| State | Border | Background | Icon |
|-------|--------|-----------|------|
| Empty | `dashed surface-border` | transparent | Custom or Upload |
| Drag over | `dashed brand-400` | `brand-500/10` + glow shadow | — |
| Staged (valid) | `dashed brand-500/40` | `brand-500/5` | Custom or Upload |
| Staged (invalid) | `dashed status-error/40` | `status-error/5` | AlertTriangle (red) |
| Uploading | — | — | Progress bar |
| Success | — | — | CheckCircle (teal) |

All states: `rounded-card border-2 border-dashed p-6 flex flex-col items-center justify-center gap-2`.

---

## Navigation

### Main Tabs (top bar)
Two tabs in the header bar: **Dashboard** and **Configuration**.
- Bar: `bg-surface-header border-b border-surface-border flex px-4`
- Active: `text-brand-400 border-b-2 border-brand-500 font-medium`
- Inactive: `text-text-secondary border-transparent hover:text-text-primary`
- Icons: Lucide `LayoutDashboard` and `Settings`, size 16

### Config Sub-Tabs (pill bar)
Four tabs within the Configuration page: **Setup**, **Scale**, **Security**, **Device**.
- Bar: `flex gap-1 rounded-card bg-surface-card border border-surface-border p-1`
- Active: `bg-brand-500/15 text-brand-400 font-medium shadow-sm rounded-[7px]`
- Inactive: `text-text-secondary hover:text-text-primary hover:bg-surface-card-hover rounded-[7px]`
- Icons: Lucide `Cpu`, `Scale`, `Shield`, `Wrench`, size 14

---

## Iconography

All icons are from [Lucide React](https://lucide.dev). Default `strokeWidth` is 2; use 1.5 for larger decorative icons (DropZone).

### Icons in use

| Icon | Context | Size |
|------|---------|------|
| `LayoutDashboard` | Dashboard tab | 16 |
| `Settings` | Configuration tab | 16 |
| `Cpu` | Setup section, Device Name | 13–16 |
| `Wifi` / `WifiOff` | WiFi status, WiFi section | 16 |
| `Shield` | Security section | 13–16 |
| `Scale` | Scale section, display, sampling | 13–16 |
| `Crosshair` | Calibration section | 16 |
| `Globe` | OTA Server section | 16 |
| `Wrench` | Device section tab | 14 |
| `Power` | Device Control section | 16 |
| `Download` | Firmware group label | 13 |
| `Upload` | Upload button, DropZone default | 14–24 |
| `HardDrive` | Backend firmware DropZone | 24 |
| `Layers` | Frontend DropZone | 24 |
| `Eye` / `EyeOff` | Password toggle | 16 |
| `RotateCcw` | Restart, Tare | 14 |
| `Trash2` | Factory Reset, Clear Cal | 14 |
| `Plus` | Add Cal Point | 14 |
| `X` | Clear/close buttons | 14 |
| `Check` | Calibrated status | 12 |
| `CheckCircle` | Upload success | 24 |
| `AlertTriangle` | Uncalibrated, validation error, confirmation | 12–24 |
| `Activity` | Event log empty state | 24 |
| `Tag` | Tags In Store empty state | 24 |
| `Pause` / `Play` | Console log pause toggle | 14 |

---

## Scrollbar

Custom dark scrollbar for log panels:
```css
.log-scroll::-webkit-scrollbar       { width: 4px; }
.log-scroll::-webkit-scrollbar-track  { background: transparent; }
.log-scroll::-webkit-scrollbar-thumb  { background: #252830; border-radius: 2px; }
```

---

## Responsive Behavior

| Breakpoint | Behavior |
|-----------|----------|
| Mobile (<640px) | Stats: 2-column grid. Logs: stacked vertically. Drop zones: stacked. |
| Tablet (640–1024px) | Stats: 4-column. Logs: side by side. Drop zones: side by side. |
| Desktop (>1024px) | Full layout. Max content width: 1100px, centered. |

---

## Selection & Focus

- Text selection: `selection:bg-brand-500/30` (amber tint)
- Focus ring on inputs: `focus:border-brand-500/50 focus:ring-1 focus:ring-brand-500/20`
- Focus on buttons: browser default (outline) — no custom override

---

## Data Presentation

### Weight Values
- Font: JetBrains Mono, semibold, `tabular-nums`
- Color: `text-data` (#f0b429)
- Unit suffix: " g" appended in the component, not the data
- Hero weight (dashboard): 4xl size with subtle amber glow shadow

### Tag IDs
- Font: JetBrains Mono, xs, normal weight
- Displayed as Badge components in a flex-wrap grid
- Hex uppercase (e.g., "B019142F", "04EE7A63C62A81")

### Log Entries
- Font: JetBrains Mono, xs
- Format: `HH:MM:SS.mmm  ▶ {"MessageType":value}` or `◀` for inbound
- Direction arrows: ▶ (U+25B6) outbound, ◀ (U+25C0) inbound
- Colored by semantic type (event/in/out/sys/err)
- Max 500 entries with auto-scroll; oldest entries pruned

### Firmware Info
- Font: JetBrains Mono, 11px, `text-text-muted`
- Format: `Flash: 16384 KB  SPIFFS: 88 / 1817 KB  Heap: 217 KB free`

---

## Confirmation Pattern

Destructive actions (Restart, Factory Reset, Clear Calibration) use an **inline confirmation** — not browser `confirm()`. The pattern:

1. User clicks danger button
2. Button is replaced by a warning panel: `rounded-card border border-status-error/30 bg-status-error/5 p-4`
3. Panel shows AlertTriangle icon + title + description
4. Two buttons: "Yes, [action]" (danger/primary) + "Cancel" (secondary)
5. On confirm: action executes, state changes to "done" with spinning icon + message

---

## File Upload Pattern

DropZone implements a **stage → validate → confirm → upload** flow:

1. **Stage**: user drops or selects a file
2. **Validate**: async validation reads file header bytes (ESP32 magic, chip ID, size limits)
3. **Review**: filename, size, validation info displayed; "Flash to device" button appears
4. **Upload**: XHR with progress tracking; progress bar + percentage
5. **Complete**: success message; device reboots

Invalid files show red border + error message; the upload button is hidden.
