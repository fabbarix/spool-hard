import { defineConfig } from 'vite';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [tailwindcss(), react()],
  build: {
    outDir: '../firmware/data',
    emptyOutDir: true,
    target: 'es2020',
    // SPIFFS caps object names at 32 bytes (CONFIG_SPIFFS_OBJ_NAME_LEN).
    // Default Vite names like "/assets/sql-wasm-UFUCzYNW.wasm.gz" (33 B)
    // silently fail to land on device. Shorten the dir to "a/" and trim the
    // hash suffix — keeps cache-busting while staying well under the cap.
    assetsDir: 'a',
    rollupOptions: {
      output: {
        assetFileNames:  'a/[name]-[hash:6][extname]',
        chunkFileNames:  'a/[name]-[hash:6].js',
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
