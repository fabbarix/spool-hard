import type { ProductMeta, FlasherManifest } from '../types';
// JSX typing for <esp-web-install-button> lives in src/esp-web-tools.d.ts.

interface Props {
  meta: ProductMeta;
  state: { loading: boolean; manifest: FlasherManifest | null; error: string | null };
}

export default function ProductCard({ meta, state }: Props) {
  return (
    <div className="rounded-card border border-border bg-card p-6 flex flex-col gap-4">
      <div>
        <h2 className="text-xl font-semibold text-text">{meta.name}</h2>
        <p className="text-sm text-text-muted mt-1">{meta.blurb}</p>
      </div>

      <div className="text-xs font-mono text-text-muted">
        {state.loading
          ? 'Loading manifest…'
          : state.manifest
          ? <span><span className="text-brand-500">v{state.manifest.version}</span> · {state.manifest.builds[0]?.chipFamily ?? 'unknown chip'}</span>
          : <span className="text-red-400">manifest unavailable ({state.error})</span>}
      </div>

      {state.manifest && !state.loading && (
        // esp-web-tools handles UI, chip detection, erase, multi-part
        // upload, progress, and reset. The slotted span is the visible
        // button label — the component wraps it in its own <button>.
        <esp-web-install-button manifest={meta.manifestPath}>
          <span slot="activate" className="cursor-pointer rounded-md bg-brand-500 hover:bg-brand-400 px-4 py-2 text-text-inverse font-medium inline-block">
            Flash {meta.name}
          </span>
          <span slot="unsupported" className="text-sm text-red-400">
            Web Serial isn't available in this browser. Use Chrome or Edge on a desktop.
          </span>
          <span slot="not-allowed" className="text-sm text-red-400">
            This page must be served over HTTPS for Web Serial to work.
          </span>
        </esp-web-install-button>
      )}

      {!state.manifest && !state.loading && (
        <div className="text-sm text-text-muted italic">
          No build available yet — wait for the next release deploy.
        </div>
      )}
    </div>
  );
}
