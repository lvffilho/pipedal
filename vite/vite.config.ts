import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import svgr from "vite-plugin-svgr"

// https://vite.dev/config/
export default defineConfig({
  build: {
    // main is ~720 kB after code-splitting (was 1.7 MB); the default 500 kB limit is not yet reachable.
    chunkSizeWarningLimit: 800,
    rollupOptions: {
      output: {
        manualChunks(id: string) {
          if (!id.includes('node_modules')) return undefined;
          if (/node_modules\/(react|react-dom|scheduler)\//.test(id)) return 'vendor-react';
          if (/node_modules\/(@mui|@emotion|tss-react|@popperjs|react-transition-group)\//.test(id)) return 'vendor-mui';
          return undefined;
        },
      },
      input: {
        main: 'index.html',
        t3k_callback: 't3k_response.html',  // your alternate page
      }
    }
  },
  plugins: [react(),svgr()],
  server: {
    proxy: {
      '/resources': {
        target: 'http://localhost:8080',
        changeOrigin: false,
      },
      '^/var/.*': {
        target: 'http://localhost:8080',
        changeOrigin: false,
      },
    }
}
})
