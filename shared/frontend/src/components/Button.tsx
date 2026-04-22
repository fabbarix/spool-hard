import type { ButtonHTMLAttributes } from 'react';

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary' | 'danger';
}

const variants: Record<NonNullable<ButtonProps['variant']>, string> = {
  primary: 'bg-brand-500 hover:bg-brand-400 text-surface-body font-semibold',
  secondary: 'bg-surface-card border border-surface-border hover:bg-surface-card-hover text-text-primary',
  danger: 'bg-red-500/10 border border-red-500/30 hover:bg-red-500/20 text-status-error',
};

export function Button({ variant = 'primary', className = '', ...props }: ButtonProps) {
  return (
    <button
      className={`rounded-button px-4 py-2 text-sm font-medium transition-all duration-200 disabled:opacity-50 ${variants[variant]} ${className}`}
      {...props}
    />
  );
}
