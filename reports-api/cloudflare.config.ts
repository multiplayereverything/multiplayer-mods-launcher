import { bindings, defineConfig } from "cf/config";
import * as entrypoint from "./src/index.ts" with { type: "cf-worker" };

// The launcher's "How did it run?" reports (src/index.ts). Not deployed until the owner says so.
export default defineConfig({
	worker: {
		name: "multiplayer-everything-reports",
		compatibilityDate: "2026-10-23",
		entrypoint,
		env: {
			DB: bindings.d1({ name: "multiplayer-everything-reports" }),
		},
	},
});
