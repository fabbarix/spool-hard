import { useState, useEffect, useCallback } from 'react';
import { QueryProvider } from '@spoolhard/ui/providers/QueryProvider';
import { AuthProvider } from '@spoolhard/ui/providers/AuthProvider';
import { WebSocketProvider } from '@spoolhard/ui/providers/WebSocketProvider';
import { ReconnectProvider } from '@spoolhard/ui/providers/ReconnectProvider';
import { ReconnectOverlay } from '@spoolhard/ui/components/ReconnectOverlay';
import { Header } from './components/layout/Header';
import { NavTabs } from './components/layout/NavTabs';
import { DashboardPage } from './pages/DashboardPage';
import { SpoolsPage } from './pages/SpoolsPage';
import { EmptyWeightsPage } from './pages/EmptyWeightsPage';
import { FilamentsPage } from './pages/FilamentsPage';
import { ConfigPage } from './pages/ConfigPage';

export type Tab = 'dashboard' | 'spools' | 'empty-weights' | 'filaments' | 'config';

function pathToTab(): Tab {
  const p = window.location.pathname;
  if (p.startsWith('/config'))         return 'config';
  if (p.startsWith('/filaments'))      return 'filaments';
  if (p.startsWith('/empty-weights'))  return 'empty-weights';
  if (p.startsWith('/spools'))         return 'spools';
  return 'dashboard';
}

const tabPaths: Record<Tab, string> = {
  dashboard:      '/',
  spools:         '/spools',
  'empty-weights':'/empty-weights',
  filaments:      '/filaments',
  config:         '/configuration',
};

export default function App() {
  const [tab, setTab] = useState<Tab>(pathToTab);

  const navigate = useCallback((t: Tab) => {
    window.history.pushState(null, '', tabPaths[t]);
    setTab(t);
  }, []);

  useEffect(() => {
    const onPop = () => setTab(pathToTab());
    window.addEventListener('popstate', onPop);
    return () => window.removeEventListener('popstate', onPop);
  }, []);

  useEffect(() => {
    const prevent = (e: DragEvent) => e.preventDefault();
    document.addEventListener('dragover', prevent);
    document.addEventListener('drop', prevent);
    return () => {
      document.removeEventListener('dragover', prevent);
      document.removeEventListener('drop', prevent);
    };
  }, []);

  return (
    <QueryProvider>
      <AuthProvider>
        <ReconnectProvider>
          <WebSocketProvider>
            {/* App shell — pin Header + NavTabs at the viewport top and let
                only <main> scroll. Earlier the whole page scrolled, which
                pushed the WiFi / Scale / WS status pills + tab nav off
                screen during long pages (Config → Device, Spools, etc).
                A viewport-height flex column with shrink-0 on the chrome
                gives a desktop-app feel and keeps the status row always
                glanceable. */}
            <div className="h-screen flex flex-col bg-surface-body font-sans text-text-primary selection:bg-brand-500/30">
              <div className="shrink-0">
                <Header />
                <NavTabs activeTab={tab} onTabChange={navigate} />
              </div>
              <main className="flex-1 overflow-y-auto">
                <div className="mx-auto max-w-[1100px] p-4">
                  {tab === 'dashboard'     && <DashboardPage />}
                  {tab === 'spools'        && <SpoolsPage />}
                  {tab === 'empty-weights' && <EmptyWeightsPage />}
                  {tab === 'filaments'     && <FilamentsPage />}
                  {tab === 'config'        && <ConfigPage />}
                </div>
              </main>
            </div>
            <ReconnectOverlay />
          </WebSocketProvider>
        </ReconnectProvider>
      </AuthProvider>
    </QueryProvider>
  );
}
