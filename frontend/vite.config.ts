import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// Desktop apps and editor extensions can exhaust Linux's per-user inotify
// instance limit. Polling avoids Vite failing at startup with EMFILE. Set
// VITE_USE_POLLING=0 on hosts with ample inotify capacity to use fs.watch.
const usePolling = process.env.VITE_USE_POLLING !== '0'

export default defineConfig({
  plugins: [vue()],
  server: {
    port: 5173,
    host: true,
    watch: {
      usePolling,
      interval: 700,
    },
  },
})
