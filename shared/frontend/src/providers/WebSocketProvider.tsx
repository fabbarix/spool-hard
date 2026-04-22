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

    function connect() {
      if (unmounted) return;
      const ws = new WebSocket(`ws://${location.host}/ws`);
      wsRef.current = ws;

      ws.onopen = () => {
        setIsConnected(true);
        retryDelay.current = 1000;
      };

      ws.onclose = () => {
        setIsConnected(false);
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
        let msg: WsMessage;
        try {
          msg = JSON.parse(ev.data);
        } catch {
          return;
        }

        const now = new Date().toLocaleTimeString();

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

    return () => {
      unmounted = true;
      if (timer) clearTimeout(timer);
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
