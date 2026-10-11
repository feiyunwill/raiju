import { useEffect, useSyncExternalStore } from "react";
import { createGatewaySource } from "./gatewaySource";
import { createMockSource } from "./mockSource";
import type {
	Dashboard,
	DashboardActions,
	DashboardSource,
	SourceMode,
} from "./types";

let source: DashboardSource | null = null;

/**
 * Picks the data source once: the demo when `VITE_DEMO=true` or when nothing is serving the API, and
 * the Raiju API otherwise. Requests always go to the page's own origin — the console in a build it
 * serves, or the dev server's proxy when `VITE_GATEWAY_URL` points at one.
 */
export function getDashboardSource(): DashboardSource {
	if (source) {
		return source;
	}
	const env = import.meta.env;
	const live =
		env.VITE_DEMO !== "true" && (env.PROD || Boolean(env.VITE_GATEWAY_URL));
	source = live
		? createGatewaySource({
				baseUrl: "",
				jsonrpcUrl: `${window.location.origin}/jsonrpc`,
			})
		: createMockSource();
	return source;
}

/** Live dashboard state, the actions that change it, and whether it comes from the demo or a console. */
export function useDashboard(): [Dashboard, DashboardActions, SourceMode] {
	const current = getDashboardSource();
	useEffect(() => {
		current.start();
		return () => current.stop();
	}, [current]);
	const state = useSyncExternalStore(current.subscribe, current.getState);
	return [state, current.actions, current.mode];
}
