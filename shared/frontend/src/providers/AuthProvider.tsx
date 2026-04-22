import { useEffect, useState, type ReactNode } from 'react';
import { useQuery, useQueryClient } from '@tanstack/react-query';
import { LoginPage } from '../components/LoginPage';
import { setStoredKey } from '../utils/authStorage';
import { installFetchWithAuth } from '../utils/installFetchWithAuth';

interface AuthStatus {
  auth_required: boolean;
  authenticated: boolean;
  device_name?: string;
  product?: string;
}

interface AuthProviderProps {
  children: ReactNode;
}

/**
 * Gates the app behind a sign-in screen when the device has a non-default
 * security key configured. Must be mounted inside QueryProvider.
 *
 * Behaviour:
 *  - Installs a window.fetch wrapper that adds Authorization to /api/*.
 *  - Fetches /api/auth-status (unauth endpoint) to check whether auth is
 *    required on this device and whether the cached key is still valid.
 *  - If required AND not authenticated → renders <LoginPage />.
 *  - On successful sign-in, stores the key and forces a refresh.
 */
export function AuthProvider({ children }: AuthProviderProps) {
  const qc = useQueryClient();
  // Install the fetch wrapper exactly once, as early as possible. Done in a
  // layout effect-safe place: the useState initializer runs synchronously on
  // the first render BEFORE any child queries fire.
  useState(() => { installFetchWithAuth(); return null; });

  const { data, isLoading, refetch } = useQuery<AuthStatus>({
    queryKey: ['auth-status'],
    queryFn: () => fetch('/api/auth-status').then((r) => r.json()),
    retry: false,
    staleTime: 30_000,
  });

  // If the stored key changes (e.g. cleared from another tab), re-probe.
  useEffect(() => {
    const onStorage = () => refetch();
    window.addEventListener('storage', onStorage);
    return () => window.removeEventListener('storage', onStorage);
  }, [refetch]);

  if (isLoading) {
    return (
      <div className="min-h-screen bg-surface-body flex items-center justify-center text-text-muted text-sm">
        Connecting…
      </div>
    );
  }

  const needsLogin = data?.auth_required && !data?.authenticated;
  if (needsLogin) {
    return (
      <LoginPage
        deviceName={data?.device_name}
        onAuthenticated={(key, remember) => {
          setStoredKey(key, remember);
          // Nuke every cached query so they refetch with the new header.
          qc.invalidateQueries();
          refetch();
        }}
      />
    );
  }

  return <>{children}</>;
}
