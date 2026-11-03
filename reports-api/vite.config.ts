import { cloudflare } from "@cloudflare/vite-plugin";
import { defineConfig } from "vite";

export default defineConfig({
	plugins: [cloudflare()],
	// A fixed port, so the launcher's local test config can point at it (tests/reports.mjs, launcher.json's reports.url).
	server: { port: 8790, strictPort: true },
});
