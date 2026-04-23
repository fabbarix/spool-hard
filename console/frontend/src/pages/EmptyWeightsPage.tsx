import { CoreWeightsSection } from '../components/config/CoreWeightsSection';

// Top-level "Empty weights" tab — surfaces the brand/material/advertised
// → core_g learning dictionary the new-spool wizard auto-populates from
// every Used / Empty capture. Used to be a sub-tab of Spools; promoted
// to its own tab so users can find + edit the data without diving into
// the Spools section.
//
// Wraps the existing CoreWeightsSection (originally written for the
// config sub-tab placement) — no behavioural change, just relocation.
export function EmptyWeightsPage() {
  return (
    <div className="space-y-4">
      <CoreWeightsSection />
    </div>
  );
}
