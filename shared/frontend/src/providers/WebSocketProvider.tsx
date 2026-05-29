import {
  createContext,
  useContext,
  useEffect,
  useRef,
  useState,
  useCallback,
  type ReactNode,
} from 'react';
import type { RawSample, WeightState, NfcEvent, ConsoleConn, LogEntry, WsMessage } from '../types/websocket';
import { queryClient } from './QueryProvider';
import { getStoredKey } from '../utils/authStorage';

// Map of `state.<resource>` wire-types to the React-Query cache key the
// firmware push should overwrite. Adding a new state resource is one
// entry here + one matching firmware producer call — no other plumbing.
//
// state.printer_analysis is the one shape with a per-key parameter
// (the printer serial), so it gets a special case in the dispatch
// rather than living in this table.
const STATE_TO_QUERY: Record<string, readonly unknown[]> = {
  'state.printers':            ['printers'],
  'state.spools':              ['spools', 0, 200, ''],
  'state.core_weights':        ['core-weights'],
  'state.scale_link':          ['scale-link'],
  'state.ota':                 ['ota-status'],
  'state.firmware_info':       ['firmware-info'],
  'state.wifi_status':         ['wifi-status'],
  'state.discovery_printers':  ['discovery-printers'],
  'state.discovery_scales':    ['discovery-scales'],
  'state.display_config':      ['display-config'],
  'state.filaments_info':      ['filaments-db-info'],
  'state.cloud_public_cache':  ['bambu-cloud-public-cache'],
};

// Query keys to invalidate on a fresh WS connect (or reconnect after a
// drop). On the first connect this is just a light resync; after a
// reconnect-from-reboot it's the load-bearing path that pulls every
// firmware-resident resource back to truth. The matching query keys
// must stay in sync with STATE_TO_QUERY above + the per-printer
// printer-analysis (handled separately via removeQueries).
const RECONNECT_INVALIDATE: readonly (readonly unknown[])[] = [
  ['printers'],
  ['scale-link'],
  ['ota-status'],
  ['firmware-info'],
  ['wifi-status'],
  ['discovery-printers'],
  ['discovery-scales'],
  ['core-weights'],
  ['filaments-db-info'],
  ['bambu-cloud-public-cache'],
  // ['spools', 0, 200, ''] is the canonical key the dashboard reads;
  // narrower views keyed by other params will refresh on their own
  // remount or via subsequent state.spools pushes.
  ['spools', 0, 200, ''],
];

// If we receive zero frames for this long, assume the device is gone
// (rebooting, TCP black-holed, network change) and force-close the WS
// to trigger the reconnect path. The firmware heart-beats state.scale_link
// once a second whenever a /ws client is attached, so any silence past
// a couple of seconds is real. 5 s is enough margin to absorb a brief
// network blip without misfiring.
const SILENCE_TIMEOUT_MS = 5000;

// Opaque structured debug payload. AMS-raw is the first consumer but
// keeping this generic lets us plumb additional debug streams later
// without another round of provider surgery.
export interface DebugEntry {
  ts: string;             // local time-of-day stamp, e.g. "14:03:22"
  type: 'ams_raw';        // union grows as we add more debug channels
  data: unknown;          // structured JSON payload, rendered by the subscriber
}

interface WebSocketContextValue {
  isConnected: boolean;
  rawSample: RawSample | null;
  weightState: WeightState | null;
  nfcEvent: NfcEvent | null;
  consoleConn: ConsoleConn | null;
  subscribeLog: (type: 'event' | 'console', cb: (entry: LogEntry) => void) => () => void;
  subscribeDebug: (cb: (entry: DebugEntry) => void) => () => void;
}

const WebSocketContext = createContext<WebSocketContextValue | null>(null);

export function useWebSocket() {
  const ctx = useContext(WebSocketContext);
  if (!ctx) throw new Error('useWebSocket must be used within WebSocketProvider');
  return ctx;
}

export function WebSocketProvider({ children }: { children: ReactNode }) {
  const [isConnected, setIsConnected] = useState(false);
  const [rawSample, setRawSample] = useState<RawSample | null>(null);
  const [weightState, setWeightState] = useState<WeightState | null>(null);
  const [nfcEvent, setNfcEvent] = useState<NfcEvent | null>(null);
  const [consoleConn, setConsoleConn] = useState<ConsoleConn | null>(null);

  const eventSubs = useRef<Set<(entry: LogEntry) => void>>(new Set());
  const consoleSubs = useRef<Set<(entry: LogEntry) => void>>(new Set());
  const debugSubs = useRef<Set<(entry: DebugEntry) => void>>(new Set());
  const wsRef = useRef<WebSocket | null>(null);
  const retryDelay = useRef(1000);

  const subscribeLog = useCallback(
    (type: 'event' | 'console', cb: (entry: LogEntry) => void) => {
      const set = type === 'event' ? eventSubs.current : consoleSubs.current;
      set.add(cb);
      return () => {
        set.delete(cb);
      };
    },
    [],
  );

  const subscribeDebug = useCallback(
    (cb: (entry: DebugEntry) => void) => {
      debugSubs.current.add(cb);
      return () => { debugSubs.current.delete(cb); };
    },
    [],
  );

  useEffect(() => {
    let unmounted = false;
    let timer: ReturnType<typeof setTimeout> | null = null;
    let silenceTimer: ReturnType<typeof setTimeout> | null = null;

    // Reset the no-traffic watchdog on every received frame. If the
    // watchdog fires, we treat the connection as dead and force a
    // close — the onclose handler then schedules the reconnect with
    // exponential backoff, which is exactly what we want for "device
    // rebooted, come back when it's up again".
    function bumpSilenceTimer() {
      if (silenceTimer) clearTimeout(silenceTimer);
      silenceTimer = setTimeout(() => {
        const ws = wsRef.current;
        if (ws && (ws.readyState === WebSocket.OPEN ||
                   ws.readyState === WebSocket.CONNECTING)) {
          // Closing here triggers ws.onclose → reconnect schedule.
          ws.close();
        }
      }, SILENCE_TIMEOUT_MS);
    }

    // Used by both the initial connect and the reconnect path to
    // pull stale React Query data back to truth + clear per-key
    // caches that have no fixed query key (printer-analysis is
    // ['printer-analysis', serial] — we don't know the serials here,
    // but invalidating on prefix removes them all).
    function resyncCaches() {
      for (const key of RECONNECT_INVALIDATE) {
        queryClient.invalidateQueries({ queryKey: key });
      }
      // Per-printer analysis caches: drop them all so any open
      // PrinterRow refetches fresh after a reboot. Matches keys that
      // start with 'printer-analysis'.
      queryClient.invalidateQueries({ queryKey: ['printer-analysis'] });
    }

    function connect() {
      if (unmounted) return;
      // Carry the stored auth key on the WS handshake. Firmware-side
      // gate (mirror of `_requireAuth` for HTTP) closes the socket
      // immediately if the key is set on the device but missing/wrong
      // here. When no key is configured the device accepts anyone, same
      // as the HTTP routes — `getStoredKey()` returning null produces
      // an empty `?key=` which the firmware treats as "no key".
      const key = getStoredKey() ?? '';
      const url = `ws://${location.host}/ws?key=${encodeURIComponent(key)}`;
      const ws = new WebSocket(url);
      wsRef.current = ws;

      ws.onopen = () => {
        setIsConnected(true);
        retryDelay.current = 1000;
        bumpSilenceTimer();
        // After a fresh connect (including reconnect), invalidate the
        // live resources so React Query refetches once via HTTP and
        // resyncs against the device's current truth. Subsequent
        // updates flow over the WS via state.* pushes.
        resyncCaches();
      };

      ws.onclose = () => {
        setIsConnected(false);
        if (silenceTimer) { clearTimeout(silenceTimer); silenceTimer = null; }
        if (!unmounted) {
          timer = setTimeout(() => {
            retryDelay.current = Math.min(retryDelay.current * 2, 10000);
            connect();
          }, retryDelay.current);
        }
      };

      ws.onerror = () => {
        ws.close();
      };

      ws.onmessage = (ev) => {
        bumpSilenceTimer();
        let msg: WsMessage;
        try {
          msg = JSON.parse(ev.data);
        } catch {
          return;
        }

        const now = new Date().toLocaleTimeString();

        // `state.<resource>` family — push-model cache replacement.
        // Handled before the typed switch so we don't have to enumerate
        // every resource in the union.
        if (typeof msg.type === 'string' && msg.type.startsWith('state.')) {
          if (msg.type === 'state.printer_analysis') {
            // Per-printer key ['printer-analysis', <serial>] — extract
            // serial from the payload and write the rest under that key.
            const data = msg.data as { serial?: string } | undefined;
            const serial = data?.serial;
            if (serial) queryClient.setQueryData(['printer-analysis', serial], data);
          } else {
            const queryKey = STATE_TO_QUERY[msg.type];
            if (queryKey) queryClient.setQueryData(queryKey, msg.data);
          }
          return;
        }

        switch (msg.type) {
          case 'raw_sample':
            setRawSample(msg.data as unknown as RawSample);
            break;
          case 'weight_state':
            setWeightState(msg.data as unknown as WeightState);
            break;
          case 'nfc': {
            setNfcEvent(msg.data as unknown as NfcEvent);
            const entry: LogEntry = {
              ts: now,
              text: `NFC: ${JSON.stringify(msg.data)}`,
              cls: 'evt',
            };
            eventSubs.current.forEach((cb) => cb(entry));
            break;
          }
          case 'console_conn':
            setConsoleConn(msg.data as unknown as ConsoleConn);
            break;
          case 'console': {
            const cls = msg.dir === 'in' ? 'in' : 'out';
            const entry: LogEntry = {
              ts: now,
              text: msg.frame ?? JSON.stringify(msg.data),
              cls,
            };
            consoleSubs.current.forEach((cb) => cb(entry));
            break;
          }
          case 'tags_in_store':
            queryClient.setQueryData(['tags-in-store'], msg.data);
            break;
          case 'ams_raw': {
            // Only delivered when the user flips Config → Debug → Log AMS on.
            // Fan-out to any subscribers; if nobody's listening it's a no-op.
            const entry: DebugEntry = { ts: now, type: 'ams_raw', data: msg.data };
            debugSubs.current.forEach((cb) => cb(entry));
            break;
          }
        }
      };
    }

    connect();

    // When the tab is hidden, browsers throttle / pause timers and the
    // WS itself stays half-alive in a way that can mask a dead device.
    // On visibility-change to visible, kick the watchdog: if no frame
    // has arrived recently, force-close to trigger an immediate
    // reconnect+resync. The user expects "switch back to the tab,
    // see fresh data" with no extra interaction.
    function onVisibility() {
      if (document.visibilityState !== 'visible') return;
      const ws = wsRef.current;
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      // Force a one-shot probe by retiming the watchdog to a much
      // shorter window. If the device is alive, the next 1 Hz heartbeat
      // beats this and the timer is reset normally; otherwise we close
      // and reconnect.
      if (silenceTimer) clearTimeout(silenceTimer);
      silenceTimer = setTimeout(() => {
        if (ws.readyState === WebSocket.OPEN) ws.close();
      }, 2000);
    }
    document.addEventListener('visibilitychange', onVisibility);

    return () => {
      unmounted = true;
      if (timer) clearTimeout(timer);
      if (silenceTimer) clearTimeout(silenceTimer);
      document.removeEventListener('visibilitychange', onVisibility);
      wsRef.current?.close();
    };
  }, []);

  return (
    <WebSocketContext.Provider
      value={{ isConnected, rawSample, weightState, nfcEvent, consoleConn, subscribeLog, subscribeDebug }}
    >
      {children}
    </WebSocketContext.Provider>
  );
}
