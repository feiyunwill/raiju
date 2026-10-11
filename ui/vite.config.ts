import path from "node:path";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig, loadEnv, type Plugin } from "vite";
import { createArtworkMiddleware } from "./src/artwork/server";

function raijuArtwork(): Plugin {
	const middleware = createArtworkMiddleware();
	return {
		name: "raiju-artwork",
		configureServer(server) {
			server.middlewares.use(middleware);
		},
		configurePreviewServer(server) {
			server.middlewares.use(middleware);
		},
	};
}

export default defineConfig(({ mode }) => {
	const environment = loadEnv(mode, import.meta.dirname, "VITE_");
	const gateway = environment.VITE_GATEWAY_URL;
	const proxy = gateway
		? {
				"/api": { target: gateway },
				"/jsonrpc": { target: gateway, ws: true },
			}
		: undefined;
	return {
		plugins: [react(), tailwindcss(), raijuArtwork()],
		resolve: {
			alias: {
				"@": path.resolve(import.meta.dirname, "./src"),
			},
		},
		server: { proxy },
		preview: { proxy },
	};
});
