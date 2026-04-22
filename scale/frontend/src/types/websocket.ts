export type WsMessageType = 'raw_sample' | 'weight_state' | 'nfc' | 'console' | 'console_conn' | 'tags_in_store';

export interface RawSample {
  weight_g: number;
  raw: number;
}

export interface WeightState {
  state: string;
  weight_g: number;
}

export interface NfcEvent {
  status: number;
  uid?: number[];
  url?: string;
  is_bambulab?: boolean;
}

export interface ConsoleConn {
  connected: boolean;
  ip?: string;
}

export interface LogEntry {
  ts: string;
  text: string;
  cls: 'evt' | 'in' | 'out' | 'sys' | 'err';
}

export interface WsMessage {
  type: WsMessageType;
  data?: Record<string, unknown>;
  dir?: 'in' | 'out';
  frame?: string;
}
