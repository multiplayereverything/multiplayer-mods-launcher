import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// Tauri expects a fixed dev port (tauri.conf.json devUrl).
export default defineConfig({
  plugins: [react()],
  clearScreen: false,
  server: {
    port: 1420,
    strictPort: true,
    // Only src/ is the page. Watching the Rust build output, the old C++ tree and the other projects in this folder
    // (tens of thousands of files) held the first page back by 20 seconds or more on every start.
    watch: { ignored: ['**/src-tauri/**', '**/old/**', '**/reports-api/**', '**/trailer/**', '**/graphify-out/**', '**/tests/**', '**/launcher/**', '**/docs/**'] },
  },
})
