import { useQuery } from '@tanstack/react-query';
import type { FirmwareInfo } from '../types/api';

export function useFirmwareInfo() {
  return useQuery<FirmwareInfo>({
    queryKey: ['firmware-info'],
    queryFn: () => fetch('/api/firmware-info').then((r) => r.json()),
  });
}
