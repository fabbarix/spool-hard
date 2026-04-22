interface BadgeProps {
  children: string;
}

export function Badge({ children }: BadgeProps) {
  return (
    <span className="inline-block rounded-badge bg-brand-500/10 text-brand-400 border border-brand-500/20 px-2.5 py-1 font-mono text-xs hover:bg-brand-500/20 hover:border-brand-500/40 transition-all duration-200 cursor-default">
      {children}
    </span>
  );
}
