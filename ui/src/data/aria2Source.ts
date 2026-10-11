import {
	Aria2Client,
	type Aria2Snapshot,
	type Aria2Status,
	syncAria2,
} from "@/aria2";
import { aria2Files, aria2ToJob } from "./aria2Adapter";
import type { Dashboard, DashboardSource, Job, JobDetail } from "./types";

const historyLength = 60;

function toBase64(buffer: ArrayBuffer): string {
	let binary = "";
	const bytes = new Uint8Array(buffer);
	for (let i = 0; i < bytes.length; i += 0x8000) {
		binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
	}
	return btoa(binary);
}

function errorText(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}

/** Live source backed by aria2 JSON-RPC, through the Raiju gateway on the PS5 or a local aria2c in development. */
export function createAria2Source(
	url: string,
	secret?: string,
): DashboardSource {
	const client = new Aria2Client({ url, secret });
	const listeners = new Set<() => void>();
	let lastSample = 0;
	let stopSync: (() => void) | null = null;
	let refresh: () => Promise<void> = async () => undefined;
	const statuses = new Map<string, Aria2Status>();
	let state: Dashboard = {
		jobs: [],
		speedHistory: Array.from({ length: historyLength }, () => 0),
		storage: null,
		console: {
			host: new URL(url, window.location.href).hostname,
			online: false,
			version: "",
			uptimeSeconds: null,
		},
		notice: null,
	};

	const set = (next: Dashboard) => {
		state = next;
		for (const listener of listeners) {
			listener();
		}
	};

	const onSnapshot = (snapshot: Aria2Snapshot) => {
		statuses.clear();
		const all = [...snapshot.active, ...snapshot.waiting, ...snapshot.stopped];
		for (const status of all) {
			statuses.set(status.gid, status);
		}
		const jobs = all.map(aria2ToJob).filter((job): job is Job => job !== null);
		const now = Date.now();
		const speedHistory =
			now - lastSample >= 900
				? [...state.speedHistory.slice(1), Number(snapshot.stat.downloadSpeed)]
				: state.speedHistory;
		if (speedHistory !== state.speedHistory) {
			lastSample = now;
		}
		set({
			...state,
			jobs,
			speedHistory,
			console: { ...state.console, online: true },
		});
	};

	const run = (action: () => Promise<unknown>) => {
		action()
			.then(() => refresh())
			.catch((error) => set({ ...state, notice: errorText(error) }));
	};

	const gidOf = (id: string) => statuses.get(id)?.gid ?? id;

	return {
		mode: "live",
		getState: () => state,
		subscribe: (listener) => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		start: () => {
			if (stopSync) {
				return;
			}
			client.connect().catch(() => undefined);
			client
				.getVersion()
				.then((version) =>
					set({
						...state,
						console: { ...state.console, version: `aria2 ${version.version}` },
					}),
				)
				.catch(() => undefined);
			const sync = syncAria2(client, onSnapshot, {
				onError: () =>
					set({ ...state, console: { ...state.console, online: false } }),
			});
			refresh = sync.refresh;
			stopSync = () => {
				sync.stop();
				client.close();
			};
		},
		stop: () => {
			stopSync?.();
			stopSync = null;
		},
		actions: {
			togglePause: (id) => {
				const status = statuses.get(id);
				if (!status) {
					return;
				}
				run(() =>
					status.status === "paused" ? client.unpause(id) : client.pause(id),
				);
			},
			pauseAll: () => run(() => client.pauseAll()),
			resumeAll: () => run(() => client.unpauseAll()),
			retry: (id) => {
				const job = state.jobs.find((item) => item.id === id);
				const uris = job?.uris ?? [];
				if (uris.length === 0) {
					set({ ...state, notice: "This download has no link to retry." });
					return;
				}
				run(async () => {
					await client.addUri(uris);
					await client.removeDownloadResult(gidOf(id));
				});
			},
			remove: (id) => {
				const status = statuses.get(id);
				const stopped =
					status && ["complete", "error", "removed"].includes(status.status);
				run(() =>
					stopped ? client.removeDownloadResult(id) : client.remove(id, true),
				);
			},
			addLinks: (links) =>
				run(() => Promise.all(links.map((link) => client.addUri([link])))),
			addTorrents: (files) =>
				run(() =>
					Promise.all(
						files
							.map(async (file) =>
								client.addTorrent(toBase64(await file.arrayBuffer())),
							)
							.values(),
					),
				),
			setSpeedLimit: (id, bytesPerSecond) =>
				run(() =>
					client.changeOption(id, {
						"max-download-limit": String(
							Math.max(0, Math.round(bytesPerSecond)),
						),
					}),
				),
			loadDetail: async (id): Promise<JobDetail | null> => {
				try {
					const status = await client.tellStatus(id);
					const job = aria2ToJob(status);
					if (!job) {
						return null;
					}
					const options = await client
						.getOption(id)
						.catch(() => ({}) as Record<string, string>);
					const isTorrent = Boolean(status.bittorrent);
					let connections: JobDetail["connections"] = [];
					if (status.status === "active") {
						connections = isTorrent
							? (await client.getPeers(id)).map((peer) => ({
									label: `${peer.ip}:${peer.port}`,
									detail: peer.seeder === "true" ? "seeder" : "peer",
									downloadSpeed: Number(peer.downloadSpeed),
									uploadSpeed: Number(peer.uploadSpeed),
								}))
							: (await client.getServers(id)).flatMap((file) =>
									file.servers.map((server) => ({
										label: new URL(server.currentUri).host,
										detail: server.currentUri,
										downloadSpeed: Number(server.downloadSpeed),
										uploadSpeed: 0,
									})),
								);
					}
					return {
						job,
						directory: status.dir,
						files: aria2Files(status),
						connections,
						connectionKind: isTorrent ? "peers" : "servers",
						maxDownloadLimit: Number(options["max-download-limit"] ?? 0),
					};
				} catch (error) {
					set({ ...state, notice: errorText(error) });
					return null;
				}
			},
			dismissNotice: () => set({ ...state, notice: null }),
		},
	};
}
