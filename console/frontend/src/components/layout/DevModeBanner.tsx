import { TriangleAlert } from 'lucide-react';
import { usePrinters } from '../../hooks/usePrinters';

// Persistent reminder on every page while any printer is in Developer Mode
// (LAN-only control). Not dismissible: it disappears when the firmware sees
// evidence the mode is off. Separate from the per-card "printer refused
// commands" notice, which covers the OFF case.
export function DevModeBanner() {
  const { data: printers } = usePrinters();
  const on = (printers ?? []).filter((p) => p.state.developer_mode === 'on');
  if (on.length === 0) return null;
  const names = on.map((p) => p.name || p.serial).join(', ');
  return (
    <div
      role="status"
      className="flex items-center gap-2 border-b border-amber-500/40 bg-amber-500/10 px-4 py-1.5 text-[12px] text-amber-300"
    >
      <TriangleAlert size={14} className="shrink-0" />
      <span>
        <strong>{names}</strong> {on.length === 1 ? 'is' : 'are'} in Developer Mode (LAN only) —
        SpoolHard can control {on.length === 1 ? 'it' : 'them'}; remember to switch back if you need Bambu Cloud.
      </span>
    </div>
  );
}
