import { useEffect, useSyncExternalStore } from "react";
import { createAria2Source } from "./aria2Source";
import { createMockSource } from "./mockSource";
import type {
	Dashboard,
	DashboardActions,
	DashboardSource,
	SourceMode,
} from "./types";

let source: DashboardSource | null = null;

/**
 * Picks the data source once: demo when `VITE_DEMO=true`, otherwise aria2 at `VITE_ARIA2_URL`, otherwise
 * the gateway's `/jsonrpc` on the serving origin in production builds, and the demo in plain development.
 */
export function getDashboardSource(): DashboardSource {
	if (source) {
		return source;
	}
	const env = import.meta.env;
	if (env.VITE_DEMO === "true") {
		source = createMockSource();
	} else if (env.VITE_ARIA2_URL) {
		source = createAria2Source(
			env.VITE_ARIA2_URL,
			env.VITE_ARIA2_SECRET || undefined,
		);
	} else if (env.PROD) {
		source = createAria2Source(`${window.location.origin}/jsonrpc`);
	} else {
		source = createMockSource();
	}
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
