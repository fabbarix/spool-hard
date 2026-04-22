import { getStoredKey } from './authStorage';

/**
 * Upload a File to the device with a progress callback. Uses XMLHttpRequest
 * (fetch lacks practical upload-progress support in all browsers) and adds
 * `Authorization: Bearer <key>` when a key is stored.
 *
 * This is the twin of installFetchWithAuth() for file uploads — without it,
 * uploads bypass the fetch wrapper and silently 401 against auth-gated
 * firmware. See scale's useFirmwareUpload / useSpiffsUpload callers.
 */
export function uploadWithAuth(
  url: string,
  file: File,
  onProgress: (pct: number) => void,
): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const form = new FormData();
    form.append('file', file);

    xhr.upload.addEventListener('progress', (e) => {
      if (e.lengthComputable) onProgress(Math.round((e.loaded / e.total) * 100));
    });
    xhr.addEventListener('load', () => {
      if (xhr.status >= 200 && xhr.status < 300) { resolve(); return; }
      if (xhr.status === 401) { reject(new Error('Upload unauthorized — re-enter the security key')); return; }
      // Try to surface a human-readable reason from the server's JSON body.
      // Firmware-side rejections (wrong product, etc.) respond with
      // {"ok":false,"error":"..."} — show that instead of a generic status.
      let msg = `Upload failed: ${xhr.status} ${xhr.statusText}`;
      try {
        const j = JSON.parse(xhr.responseText);
        if (j && typeof j.error === 'string' && j.error) msg = j.error;
      } catch { /* body wasn't JSON; fall back to status text */ }
      reject(new Error(msg));
    });
    xhr.addEventListener('error', () => reject(new Error('Upload network error')));
    xhr.open('POST', url);

    const key = getStoredKey();
    if (key) xhr.setRequestHeader('Authorization', `Bearer ${key}`);

    xhr.send(form);
  });
}
