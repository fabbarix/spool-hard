import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';

export interface SecurityKeyStatus { configured: boolean; key_preview: string; }

export function useSecurityKey() {
  return useQuery<SecurityKeyStatus>({
    queryKey: ['security-key'],
    queryFn: () => fetch('/api/test-key').then((r) => r.json()),
  });
}

export function useSetSecurityKey() {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: (body: { key: string }) =>
      fetch('/api/fixed-key-config', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      }),
    onSuccess: () => qc.invalidateQueries({ queryKey: ['security-key'] }),
  });
}
