import { TriangleAlert } from 'lucide-react';
import { usePrinters } from '../../hooks/usePrinters';

// Persistent reminder on every page while any printer is in Developer Mode
// (LAN-only control). Not dismissible: it disappears when the firmware sees
// evidence the mode is off (probed on every printer connect). Separate from the per-card "printer refused
// commands" notice, which covers the OFF case.
export function DevModeBanner() {
  const { data: printers } = usePrinters();
  const on = (printers ?? []).filter((p) => p.state.developer_mode === 'on');
  if (on.length === 0) return null;
  const names = on.map((p) => p.name || p.serial).join(', ');
  return (
    // Same column as the header / tabs / page content.
    <div className="mx-auto w-full max-w-[1100px] px-4 pt-3">
      <div
        role="status"
        className="flex items-center gap-2 rounded-card border border-amber-500/40 bg-amber-500/10 px-3 py-2 text-xs text-amber-300"
      >
        <TriangleAlert size={14} className="shrink-0" />
        <span>
          <strong>{names}</strong> {on.length === 1 ? 'is' : 'are'} in Developer Mode (LAN only) —
          SpoolHard can control {on.length === 1 ? 'it' : 'them'}; remember to switch back if you need Bambu Cloud.
        </span>
      </div>
    </div>
  );
}
