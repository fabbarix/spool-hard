import { useQuery } from '@tanstack/react-query';

export interface FirmwareInfo {
  fw_version: string;
  fe_version: string;
  flash_size: number;
  spiffs_total: number;
  spiffs_used: number;
  userfs_total: number;
  userfs_used: number;
  free_heap: number;
  psram_free: number;
  sd_mounted: boolean;
  sd_total: number;
  sd_used: number;
  // Lifetime low-water-marks. Strictly monotonic — a declining trend
  // over time = real leak (vs. allocation churn that makes free_heap
  // swing 15 K every few seconds).
  min_free_heap?:    number;
  min_free_psram?:   number;
  max_alloc_heap?:   number;
  max_alloc_psram?:  number;
  // Seconds since boot. Resets on reboot, OTA, or panic.
  uptime_s?: number;
}

export function useFirmwareInfo() {
  return useQuery<FirmwareInfo>({
    queryKey: ['firmware-info'],
    queryFn: () => fetch('/api/firmware-info').then((r) => r.json()),
    // Push-driven via WS `state.firmware_info` — fired periodically
    // from the firmware's main loop, rate-gated 30s. Heap fluctuates
    // continuously so a finer cadence isn't useful for the UI.
  });
}
