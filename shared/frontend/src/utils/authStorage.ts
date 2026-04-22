// Per-origin auth-key storage. Each device (e.g. spoolhard-scale.local,
// spoolhard-console.local, 10.0.0.42, …) is a distinct origin so there's
// no cross-device bleed.
const STORAGE_KEY = 'spoolhard.auth';

export function getStoredKey(): string | null {
  if (typeof window === 'undefined') return null;
  return window.localStorage.getItem(STORAGE_KEY)
      ?? window.sessionStorage.getItem(STORAGE_KEY);
}

export function setStoredKey(key: string, remember: boolean): void {
  if (typeof window === 'undefined') return;
  if (remember) {
    window.localStorage.setItem(STORAGE_KEY, key);
    window.sessionStorage.removeItem(STORAGE_KEY);
  } else {
    window.sessionStorage.setItem(STORAGE_KEY, key);
    window.localStorage.removeItem(STORAGE_KEY);
  }
}

export function clearStoredKey(): void {
  if (typeof window === 'undefined') return;
  window.localStorage.removeItem(STORAGE_KEY);
  window.sessionStorage.removeItem(STORAGE_KEY);
}
