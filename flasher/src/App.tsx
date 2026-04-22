import { useEffect, useState } from 'react';
import ProductCard from './components/ProductCard';
import type { ProductMeta, FlasherManifest } from './types';

// Repo coordinates for the "view source" link. The flasher itself reads
// manifests from `./latest/` (bundled into the Pages deploy) — see
// .github/workflows/deploy-flasher.yml — so it doesn't hit GitHub at all
// at runtime, sidestepping the release-CDN's missing CORS headers.
const OWNER = 'fabbarix';
const REPO  = 'spool-hard';

const PRODUCTS: ProductMeta[] = [
  {
    id: 'console',
    name: 'Console',
    blurb: 'Wall-mounted touchscreen — Bambu MQTT, NFC, scale link, spool DB.',
    manifestPath: 'latest/spoolhard-console-flasher-manifest.json',
  },
  {
    id: 'scale',
    name: 'Scale',
    blurb: 'Load-cell weighing platform with NFC and protocol-WS server.',
    manifestPath: 'latest/spoolhard-scale-flasher-manifest.json',
  },
];

interface ProductState {
  loading: boolean;
  manifest: FlasherManifest | null;
  error: string | null;
}

export default function App() {
  // Per-product manifest state. Each card needs its own version label,
  // and the load can fail independently (e.g. only one product was
  // built into the latest release).
  const [state, setState] = useState<Record<string, ProductState>>(
    () => Object.fromEntries(PRODUCTS.map((p) => [p.id, { loading: true, manifest: null, error: null }])),
  );

  useEffect(() => {
    PRODUCTS.forEach((p) => {
      fetch(p.manifestPath)
        .then((r) => {
          if (!r.ok) throw new Error(`${r.status}`);
          return r.json() as Promise<FlasherManifest>;
        })
        .then((m) => setState((s) => ({ ...s, [p.id]: { loading: false, manifest: m, error: null } })))
        .catch((e: Error) => setState((s) => ({ ...s, [p.id]: { loading: false, manifest: null, error: e.message } })));
    });
  }, []);

  // Pick a single version label for the header — both products are
  // released in lockstep so they should match. Fall back to the first
  // available manifest if one is missing.
  const headerVersion =
    state.console.manifest?.version ?? state.scale.manifest?.version ?? null;

  return (
    <div className="min-h-screen bg-body text-text">
      <header className="border-b border-border bg-card">
        <div className="mx-auto max-w-3xl px-6 py-6">
          <h1 className="text-2xl font-semibold">SpoolHard Flasher</h1>
          <p className="text-sm text-text-muted mt-1">
            Connect a Console or Scale board over USB and flash the latest release directly from your browser.
          </p>
          {headerVersion && (
            <p className="text-xs font-mono text-text-muted mt-2">
              latest: <span className="text-brand-500">v{headerVersion}</span>
            </p>
          )}
        </div>
      </header>

      <main className="mx-auto max-w-3xl px-6 py-8 flex flex-col gap-6">
        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
          {PRODUCTS.map((p) => (
            <ProductCard key={p.id} meta={p} state={state[p.id]} />
          ))}
        </div>

        <section className="rounded-card border border-border bg-card p-6 text-sm text-text-muted leading-relaxed">
          <h2 className="text-base font-semibold text-text mb-2">Before you flash</h2>
          <ul className="list-disc pl-5 space-y-1">
            <li>Use Chrome or Edge on desktop. Firefox / Safari don't ship Web Serial.</li>
            <li>Connect the board's native USB port directly — no hubs, no charging-only cables.</li>
            <li>The flasher will erase the chip and write a fresh image. Saved WiFi / calibration / pairing in NVS will be wiped.</li>
            <li>Total flash time: ~30 s per board.</li>
          </ul>
          <p className="mt-4 text-xs">
            Source:{' '}
            <a className="text-brand-500 hover:underline" href={`https://github.com/${OWNER}/${REPO}`} target="_blank" rel="noreferrer">
              {OWNER}/{REPO}
            </a>
            . Releases:{' '}
            <a className="text-brand-500 hover:underline" href={`https://github.com/${OWNER}/${REPO}/releases`} target="_blank" rel="noreferrer">
              all tags
            </a>
            .
          </p>
        </section>
      </main>
    </div>
  );
}
