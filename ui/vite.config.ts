import path from "node:path";
import tailwindcss from "@tailwindcss/vite";
import react from "@vitejs/plugin-react";
import { defineConfig, type Plugin } from "vite";
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

export default defineConfig({
	plugins: [react(), tailwindcss(), raijuArtwork()],
	resolve: {
		alias: {
			"@": path.resolve(import.meta.dirname, "./src"),
		},
	},
});
