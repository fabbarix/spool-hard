// GitHub Releases REST API — only the fields we actually consume.
// Reference: https://docs.github.com/en/rest/releases/releases#get-the-latest-release
export interface GithubReleaseAsset {
  name: string;
  browser_download_url: string;
  size: number;
}

export interface GithubRelease {
  tag_name: string;
  name: string;
  html_url: string;
  published_at: string;
  prerelease: boolean;
  draft: boolean;
  assets: GithubReleaseAsset[];
}

export type ProductId = 'console' | 'scale';

export interface ProductMeta {
  id: ProductId;
  name: string;
  blurb: string;
  // Filename pattern in the GitHub release the per-product flasher manifest
  // is uploaded under. release.sh writes it as `flasher-manifest.json` inside
  // each product's release/ subdir; the GitHub-Release upload step is
  // expected to rename to this prefix when attaching multiple products to
  // the same release.
  manifestAssetName: string;
}
