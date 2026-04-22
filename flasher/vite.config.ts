import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

// Deployed at https://flasher.spoolhard.io (or whatever CNAME maps to
// the gh-pages site), so the SPA can keep its assets at the site root.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  base: './',
  build: { outDir: 'dist', sourcemap: true },
  server: { port: 5173, host: true },
});
