export type WsMessageType =
  | 'raw_sample'
  | 'weight_state'
  | 'nfc'
  | 'console'
  | 'console_conn'
  | 'tags_in_store'
  | 'ams_raw'      // debug stream, user-gated via /api/debug/config
  // `state.<resource>` family — push-model resource updates that get
  // dropped straight into React Query's cache. The provider's onmessage
  // handler matches `msg.type.startsWith('state.')` and routes via a
  // resource→queryKey table, so adding a new state resource needs no
  // change to this type union (it's narrowed only for the dispatch's
  // own type-check). String here so we don't have to enumerate every
  // resource — the firmware decides which it pushes.
  | `state.${string}`;

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
