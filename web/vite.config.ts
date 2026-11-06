import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// In development the browser talks to Vite; /api and /healthz are proxied to
// the C++ gateway so the app is same-origin in dev and behind Nginx alike.
// 127.0.0.1, not localhost: Node resolves localhost to ::1 first, and another
// service on [::1]:8080 would silently answer instead of the gateway.
const gateway = process.env.CARTWHEEL_GATEWAY ?? 'http://127.0.0.1:8080';

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: {
      '/api': { target: gateway, ws: true },
      '/healthz': gateway,
    },
  },
});
