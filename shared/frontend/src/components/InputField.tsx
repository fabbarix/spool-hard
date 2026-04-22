import type { InputHTMLAttributes } from 'react';

interface InputFieldProps extends InputHTMLAttributes<HTMLInputElement> {
  label: string;
}

export function InputField({ label, className = '', ...props }: InputFieldProps) {
  return (
    <label className="block text-sm">
      <span className="mb-1 block text-sm text-text-secondary">{label}</span>
      <input
        className={`w-full bg-surface-input border border-surface-border rounded-button px-3 py-2 text-sm text-text-primary placeholder:text-text-muted focus:outline-none focus:border-brand-500/50 focus:ring-1 focus:ring-brand-500/20 transition-colors ${className}`}
        {...props}
      />
    </label>
  );
}
