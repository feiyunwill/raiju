import type { Aria2Client } from "./client";
import { pollWhileVisible } from "./poller";
import type { Aria2GlobalStat, Aria2Status, Aria2StatusKey } from "./types";

export type Aria2Snapshot = {
	active: Aria2Status[];
	waiting: Aria2Status[];
	stopped: Aria2Status[];
	stat: Aria2GlobalStat;
	updatedAt: number;
};

export type Aria2SyncOptions = {
	intervalMs?: number;
	listLimit?: number;
	keys?: Aria2StatusKey[];
	visibility?: Pick<
		Document,
		"visibilityState" | "addEventListener" | "removeEventListener"
	>;
	onError?: (error: unknown) => void;
};

export type Aria2Sync = {
	refresh: () => Promise<void>;
	stop: () => void;
};

export const defaultStatusKeys: Aria2StatusKey[] = [
	"gid",
	"status",
	"totalLength",
	"completedLength",
	"downloadSpeed",
	"uploadSpeed",
	"connections",
	"errorCode",
	"errorMessage",
	"files",
	"bittorrent",
	"followedBy",
	"following",
];

/**
 * Keeps a snapshot of aria2 state current: active downloads and global stats are polled while the page
 * is visible, and every download notification triggers a full refresh of active, waiting and stopped lists.
 */
export function syncAria2(
	client: Aria2Client,
	onSnapshot: (snapshot: Aria2Snapshot) => void,
	options: Aria2SyncOptions = {},
): Aria2Sync {
	const keys = options.keys ?? defaultStatusKeys;
	const limit = options.listLimit ?? 1000;
	let snapshot: Aria2Snapshot | null = null;

	const refresh = async () => {
		try {
			const [active, waiting, stopped, stat] = await Promise.all([
				client.tellActive(keys),
				client.tellWaiting(0, limit, keys),
				client.tellStopped(0, limit, keys),
				client.getGlobalStat(),
			]);
			snapshot = { active, waiting, stopped, stat, updatedAt: Date.now() };
			onSnapshot(snapshot);
		} catch (error) {
			options.onError?.(error);
		}
	};

	const tick = async () => {
		if (!snapshot) {
			await refresh();
			return;
		}
		try {
			const [active, stat] = await Promise.all([
				client.tellActive(keys),
				client.getGlobalStat(),
			]);
			snapshot = { ...snapshot, active, stat, updatedAt: Date.now() };
			onSnapshot(snapshot);
		} catch (error) {
			options.onError?.(error);
		}
	};

	const unsubscribe = client.onEvent(() => {
		void refresh();
	});
	const poller = pollWhileVisible(
		tick,
		options.intervalMs ?? 1000,
		options.visibility,
	);

	return {
		refresh,
		stop: () => {
			unsubscribe();
			poller.stop();
		},
	};
}
