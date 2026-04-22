interface StatusDotProps {
  status: 'connected' | 'connecting' | 'disconnected';
}

const dotStyles: Record<StatusDotProps['status'], string> = {
  connected: 'bg-status-connected shadow-[0_0_6px_rgba(45,212,191,0.5)]',
  connecting: 'bg-status-connecting animate-pulse-glow',
  disconnected: 'bg-status-disconnected',
};

export function StatusDot({ status }: StatusDotProps) {
  return <span className={`inline-block h-2.5 w-2.5 rounded-full ${dotStyles[status]}`} />;
}
