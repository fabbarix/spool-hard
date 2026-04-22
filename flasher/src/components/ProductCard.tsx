import type { GithubRelease, ProductMeta } from '../types';
// JSX typing for <esp-web-install-button> lives in src/esp-web-tools.d.ts.

interface Props {
  meta: ProductMeta;
  release: GithubRelease | null;
}

// Find an asset by name in a release's asset list. GitHub release-asset
// downloads come from objects.githubusercontent.com which sets CORS-permissive
// headers, so esp-web-tools can fetch them directly from the browser.
function findAsset(release: GithubRelease | null, name: string) {
  return release?.assets.find((a) => a.name === name) ?? null;
}

export default function ProductCard({ meta, release }: Props) {
  const manifestAsset = findAsset(release, meta.manifestAssetName);
  const manifestUrl = manifestAsset?.browser_download_url ?? '';

  return (
    <div className="rounded-card border border-border bg-card p-6 flex flex-col gap-4">
      <div>
        <h2 className="text-xl font-semibold text-text">{meta.name}</h2>
        <p className="text-sm text-text-muted mt-1">{meta.blurb}</p>
      </div>

      <div className="text-xs font-mono text-text-muted">
        {release ? (
          <>
            <span className="text-brand-500">{release.tag_name}</span>
            <span className="mx-2">·</span>
            {new Date(release.published_at).toLocaleDateString()}
            {release.prerelease && (
              <span className="ml-2 px-2 py-0.5 rounded bg-amber-900/30 text-amber-400">
                pre-release
              </span>
            )}
          </>
        ) : (
          'Loading release info…'
        )}
      </div>

      {manifestUrl ? (
        // esp-web-tools handles UI, chip detection, erase, multi-part
        // upload, progress, and reset. The slotted span is the visible
        // button label — the component wraps it in its own <button>.
        <esp-web-install-button manifest={manifestUrl}>
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
      ) : release ? (
        <div className="text-sm text-text-muted italic">
          No <code className="font-mono">{meta.manifestAssetName}</code> asset in this release.
        </div>
      ) : null}
    </div>
  );
}
