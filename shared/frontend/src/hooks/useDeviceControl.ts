import { useMutation } from '@tanstack/react-query';

export function useRestart() {
  return useMutation({
    mutationFn: () => fetch('/api/restart', { method: 'POST' }),
  });
}

export function useFactoryReset() {
  return useMutation({
    mutationFn: () => fetch('/api/reset-device', { method: 'POST' }),
  });
}
