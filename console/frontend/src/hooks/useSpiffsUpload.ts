import { useMutation } from '@tanstack/react-query';
import { useState } from 'react';
import { uploadWithAuth } from '@spoolhard/ui/utils/uploadWithAuth';

// The firmware answers the POST once the bytes are in (202) and verifies the
// image (flash readback + remount) in a background task. Poll the status
// endpoint so the UI reports the real end result. After "ok" the device
// reboots, so a dropped connection is expected: success is only reported once
// the device is seen again (status "ok", or "idle" after the reboot). A
// connection that never recovers within the deadline is a failure.
async function waitForVerify(): Promise<void> {
  const deadline = Date.now() + 90_000;
  let wentDown = false;
  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, 1000));
    let j: { phase?: string; error?: string };
    try {
      const res = await fetch('/api/upload/spiffs/status');
      if (!res.ok) continue;
      j = await res.json();
    } catch {
      wentDown = true; // rebooting (or genuinely gone: the deadline decides)
      continue;
    }
    if (j.phase === 'ok') return;
    if (j.phase === 'failed') throw new Error(j.error || 'Frontend verification failed');
    if (j.phase === 'idle' && wentDown) return; // came back after the reboot
  }
  throw new Error('Timed out waiting for the device to verify the frontend');
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
