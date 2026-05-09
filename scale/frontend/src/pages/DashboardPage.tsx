import { StatsRow } from '../components/dashboard/StatsRow';
import { TagsInStore } from '../components/dashboard/TagsInStore';

// Dashboard is the at-a-glance view: weight, NFC, console link state,
// uptime, and the spool inventory the scale knows about. The verbose
// streams (event log + console-frame log) moved to Config → Debug so
// the home page stays focused on what the user is actually here to
// see, mirroring how the console product organises its own UI.
export function DashboardPage() {
  return (
    <div className="space-y-4">
      <div className="animate-in" style={{ animationDelay: '0ms' }}><StatsRow /></div>
      <div className="animate-in" style={{ animationDelay: '100ms' }}><TagsInStore /></div>
    </div>
  );
}
