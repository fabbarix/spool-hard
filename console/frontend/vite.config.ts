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
    // (Rolldown requires >=6 hash chars, so lazy chunks are named by hash only: "[name]-[hash:6]" for EmptyWeightsPage was 32.)
    // SPIFFS caps object names at 31 chars incl. the leading '/' and the
    // '.gz' release.sh appends (CONFIG_SPIFFS_OBJ_NAME_LEN=32 with NUL).
    // Default Vite names like "/assets/some-long-asset-name.css.gz" can run
    // over 32 B and silently fail to land on device. Shorten the dir to "a/"
    // and trim the hash suffix — keeps cache-busting while staying well
    // under the cap. (Originally introduced for the sql-wasm asset that has
    // since been removed; the rule still matters for future large assets.)
    assetsDir: 'a',
    rollupOptions: {
      output: {
        assetFileNames:  'a/[name]-[hash:6][extname]',
        chunkFileNames:  'a/[hash:6].js',
        entryFileNames:  'a/[name]-[hash:6].js',
      },
    },
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
