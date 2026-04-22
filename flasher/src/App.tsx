import { useEffect, useState } from 'react';
import ProductCard from './components/ProductCard';
import type { GithubRelease, ProductMeta } from './types';

// Repo coordinates. Edit here if the repo ever moves; everything else
// derives from this.
const OWNER = 'fabbarix';
const REPO  = 'spool-hard';
const RELEASES_URL = `https://api.github.com/repos/${OWNER}/${REPO}/releases/latest`;

const PRODUCTS: ProductMeta[] = [
  {
    id: 'console',
    name: 'Console',
    blurb: 'Wall-mounted touchscreen — Bambu MQTT, NFC, scale link, spool DB.',
    manifestAssetName: 'spoolhard-console-flasher-manifest.json',
  },
  {
    id: 'scale',
    name: 'Scale',
    blurb: 'Load-cell weighing platform with NFC and protocol-WS server.',
    manifestAssetName: 'spoolhard-scale-flasher-manifest.json',
  },
];

export default function App() {
  const [release, setRelease] = useState<GithubRelease | null>(null);
  const [error, setError]     = useState<string | null>(null);

  useEffect(() => {
    // GitHub's REST API sets `Access-Control-Allow-Origin: *` so the call
    // from a Pages origin works without a proxy. 60 req/h unauthenticated
    // — fine for a one-off page hit; we cache nothing, so a refresh costs
    // one request.
    fetch(RELEASES_URL)
      .then((r) => {
        if (!r.ok) throw new Error(`GitHub API ${r.status}`);
        return r.json();
      })
      .then((j: GithubRelease) => setRelease(j))
      .catch((e: Error) => setError(e.message));
  }, []);

  return (
    <div className="min-h-screen bg-body text-text">
      <header className="border-b border-border bg-card">
        <div className="mx-auto max-w-3xl px-6 py-6">
          <h1 className="text-2xl font-semibold">SpoolHard Flasher</h1>
          <p className="text-sm text-text-muted mt-1">
            Connect a Console or Scale board over USB and flash the latest release directly from your browser.
          </p>
        </div>
      </header>

      <main className="mx-auto max-w-3xl px-6 py-8 flex flex-col gap-6">
        {error && (
          <div className="rounded-card border border-red-700/40 bg-red-950/30 p-4 text-sm text-red-300">
            Couldn't fetch the latest release ({error}). The flash buttons will appear once the GitHub API is reachable.
          </div>
        )}

        <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
          {PRODUCTS.map((p) => (
            <ProductCard key={p.id} meta={p} release={release} />
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
          {release && (
            <p className="mt-4 text-xs">
              Pulling artifacts from{' '}
              <a className="text-brand-500 hover:underline" href={release.html_url} target="_blank" rel="noreferrer">
                {release.tag_name}
              </a>
              . Source:{' '}
              <a className="text-brand-500 hover:underline" href={`https://github.com/${OWNER}/${REPO}`} target="_blank" rel="noreferrer">
                {OWNER}/{REPO}
              </a>
              .
            </p>
          )}
        </section>
      </main>
    </div>
  );
}
