import { getStoredKey } from './authStorage';

let installed = false;

/**
 * Monkey-patches window.fetch so every request to /api/* automatically
 * carries an Authorization: Bearer <key> header when a key is stored. Leaves
 * /captive/* and any non-API URL unchanged. Safe to call multiple times.
 *
 * This keeps existing hook code (`fetch('/api/foo')`) untouched — the wrapper
 * only acts on API URLs and only when a key is present in storage.
 */
export function installFetchWithAuth(): void {
  if (installed || typeof window === 'undefined') return;
  installed = true;

  const originalFetch = window.fetch.bind(window);

  window.fetch = function (input: RequestInfo | URL, init?: RequestInit): Promise<Response> {
    const url =
      typeof input === 'string' ? input :
      input instanceof URL ? input.pathname + input.search :
      input.url;

    // Only touch our API routes — leave third-party fetches and the captive
    // portal alone.
    const isApi = url.startsWith('/api/') || url.includes('/api/');
    if (!isApi) return originalFetch(input, init);

    const key = getStoredKey();
    if (!key) return originalFetch(input, init);

    const baseHeaders =
      init?.headers ??
      (typeof input !== 'string' && !(input instanceof URL) ? input.headers : undefined);
    const headers = new Headers(baseHeaders);
    if (!headers.has('Authorization')) {
      headers.set('Authorization', `Bearer ${key}`);
    }
    return originalFetch(input, { ...init, headers });
  };
}
