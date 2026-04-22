// esp-web-tools manifest format. We only read the version for display;
// esp-web-tools handles parsing the rest itself.
export interface FlasherManifest {
  name: string;
  version: string;
  new_install_prompt_erase?: boolean;
  builds: Array<{
    chipFamily: string;
    parts: Array<{ path: string; offset: number }>;
  }>;
}

export type ProductId = 'console' | 'scale';

export interface ProductMeta {
  id: ProductId;
  name: string;
  blurb: string;
  // Same-origin path the Pages deploy serves the per-product manifest at.
  // The deploy-flasher workflow pulls the latest release's
  // `spoolhard-<product>-flasher-manifest.json` into `latest/` and
  // rewrites its bin URLs to relative names that resolve against the
  // manifest's own location — so loading this manifest URL is enough.
  manifestPath: string;
}
