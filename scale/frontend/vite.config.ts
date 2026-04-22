import { defineConfig } from 'vite';
import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [tailwindcss(), react()],
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
