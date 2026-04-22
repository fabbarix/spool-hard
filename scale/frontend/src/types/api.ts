export interface WifiStatus {
  configured: boolean;
  connected: boolean;
  ssid: string;
  ip: string;
  rssi: number;
}

export interface DeviceNameConfig {
  device_name: string;
}

export interface OtaConfig {
  url: string;
  use_ssl: boolean;
  verify_ssl: boolean;
}

export interface SecurityKeyStatus {
  configured: boolean;
  key_preview: string;
}

export interface TagsInStore {
  tags: string;
}

export interface FirmwareInfo {
  fw_version: string;
  fe_version: string;
  flash_size: number;
  spiffs_total: number;
  spiffs_used: number;
  free_heap: number;
}

export interface WifiNetwork {
  ssid: string;
  rssi: number;
  secure: boolean;
}

export interface CalPoint {
  // ADC counts above the current tare at the moment this point was
  // captured — i.e. the load cell's slope at weight_g. Stays valid
  // across re-tares because it's tare-relative by construction.
  delta: number;
  weight_g: number;
}

export interface ScaleConfig {
  samples: number;
  stable_threshold: number;
  stable_count: number;
  load_detect: number;
  precision: number;
  rounding: 'round' | 'truncate';
  calibrated: boolean;
  tare_raw: number;
  num_points: number;
  points?: CalPoint[];
}
