import { StatsRow } from '../components/dashboard/StatsRow';
import { EventLog } from '../components/dashboard/EventLog';
import { ConsoleLog } from '../components/dashboard/ConsoleLog';
import { TagsInStore } from '../components/dashboard/TagsInStore';

export function DashboardPage() {
  return (
    <div className="space-y-4">
      <div className="animate-in" style={{ animationDelay: '0ms' }}><StatsRow /></div>
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <div className="animate-in" style={{ animationDelay: '100ms' }}><EventLog /></div>
        <div className="animate-in" style={{ animationDelay: '150ms' }}><ConsoleLog /></div>
      </div>
      <div className="animate-in" style={{ animationDelay: '200ms' }}><TagsInStore /></div>
    </div>
  );
}
