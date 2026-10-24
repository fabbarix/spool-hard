import { useMutation } from '@tanstack/react-query';
import { useState } from 'react';
import { uploadWithAuth } from '@spoolhard/ui/utils/uploadWithAuth';

// The firmware answers the POST once the bytes are in (202) and verifies the
// image (flash readback + remount) in a background task. Poll the status
// endpoint so the UI reports the real end result. The device reboots shortly
// after "ok", so a dropped connection while polling also counts as success
// (a failed upload never reboots).
async function waitForVerify(): Promise<void> {
  const deadline = Date.now() + 60_000;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 1000));
    let j: { phase?: string; error?: string };
    try {
      const res = await fetch('/api/upload/spiffs/status');
      if (!res.ok) continue;
      j = await res.json();
    } catch {
      return; // device rebooting
    }
    if (j.phase === 'ok') return;
    if (j.phase === 'failed') throw new Error(j.error || 'Frontend verification failed');
  }
  throw new Error('Timed out waiting for frontend verification');
}

export function useSpiffsUpload() {
  const [progress, setProgress] = useState(0);
  const mutation = useMutation({
    mutationFn: async (file: File) => {
      await uploadWithAuth('/api/upload/spiffs', file, setProgress);
      await waitForVerify();
    },
    onSettled: () => setProgress(0),
  });
  return { ...mutation, progress };
}
