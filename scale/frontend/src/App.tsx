import { useState, useEffect, useCallback } from 'react';
import { QueryProvider } from '@spoolhard/ui/providers/QueryProvider';
import { AuthProvider } from '@spoolhard/ui/providers/AuthProvider';
import { WebSocketProvider } from '@spoolhard/ui/providers/WebSocketProvider';
import { ReconnectProvider } from '@spoolhard/ui/providers/ReconnectProvider';
import { ReconnectOverlay } from '@spoolhard/ui/components/ReconnectOverlay';
import { Header } from './components/layout/Header';
import { NavTabs } from './components/layout/NavTabs';
import { DashboardPage } from './pages/DashboardPage';
import { ConfigPage } from './pages/ConfigPage';

type Tab = 'dashboard' | 'config';

function pathToTab(): Tab {
  const p = window.location.pathname;
  if (p === '/configuration' || p === '/config') return 'config';
  return 'dashboard';
}

const tabPaths: Record<Tab, string> = {
  dashboard: '/',
  config: '/configuration',
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

  // Prevent browser from opening dropped files as navigation.
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
            {/* App shell — pin Header + NavTabs at the viewport top, only
                <main> scrolls. Same shape as the console's App.tsx; keeps
                the WiFi / link / WS pills and tab row always visible
                regardless of how long the active page is. */}
            <div className="h-screen flex flex-col bg-surface-body font-sans text-text-primary selection:bg-brand-500/30">
              <div className="shrink-0">
                <Header />
                <NavTabs activeTab={tab} onTabChange={navigate} />
              </div>
              <main className="flex-1 overflow-y-auto">
                <div className="mx-auto max-w-[1100px] p-4">
                  {tab === 'dashboard' ? <DashboardPage /> : <ConfigPage />}
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
