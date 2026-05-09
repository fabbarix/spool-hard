import { Bug } from 'lucide-react';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { EventLog } from '../dashboard/EventLog';
import { ConsoleLog } from '../dashboard/ConsoleLog';

// Debug tools — moved off the dashboard so the home view stays focused
// on weight + scale state. Mirrors the console product's Config →
// Debug location so the two products feel consistent. Both panels are
// session-only (their underlying WS subscriptions live on the shared
// WebSocketProvider) — closing the tab clears their state.
export function DebugSection() {
  return (
    <SectionCard
      title="Debug"
      icon={<Bug size={16} />}
      description="Live event + console-frame streams from the firmware. Useful when diagnosing protocol issues or verifying NFC reads. Closing this tab stops capture; nothing persists across reloads."
    >
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <EventLog />
        <ConsoleLog />
      </div>
    </SectionCard>
  );
}
