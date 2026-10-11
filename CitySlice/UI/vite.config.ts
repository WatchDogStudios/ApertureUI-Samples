import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// Output shape the engine expects: index.html next to assets/, document-relative
// paths (Aperture resolves ./assets/... against the document), one module script,
// one stylesheet, stable file names so the game can load UI/dist/index.html.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2020',
    modulePreload: false,
    cssCodeSplit: false,
    assetsInlineLimit: 1_000_000,
    rollupOptions: {
      output: {
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name][extname]',
      },
    },
  },
})
