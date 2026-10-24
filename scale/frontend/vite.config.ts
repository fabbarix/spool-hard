import { defineConfig, type Plugin } from 'vite';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';

// Post-build guard: SPIFFS object names max out at 31 chars including the
// leading '/', and release.sh adds '.gz'. A longer name makes mkspiffs fail
// (error -10010) and every later file silently goes missing.
const spiffsNameGuard = (): Plugin => ({
  name: 'spiffs-name-guard',
  writeBundle(_o, bundle) {
    const bad = Object.keys(bundle).filter((f) => `/${f}.gz`.length > 31);
    if (bad.length) throw new Error(`SPIFFS name > 31 chars: ${bad.map((f) => `/${f}.gz`).join(', ')}`);
  },
});

export default defineConfig({
  plugins: [tailwindcss(), react(), spiffsNameGuard()],
  build: {
    outDir: '../firmware/data',
    emptyOutDir: true,
    target: 'es2020',
  },
  server: {
    proxy: {
      '/api': {
        target: `http://${process.env.VITE_DEVICE_IP || '192.168.4.1'}`,
        changeOrigin: true,
      },
      '/captive': {
        target: `http://${process.env.VITE_DEVICE_IP || '192.168.4.1'}`,
        changeOrigin: true,
      },
      '/ws': {
        target: `http://${process.env.VITE_DEVICE_IP || '192.168.4.1'}`,
        ws: true,
      },
    },
  },
});
