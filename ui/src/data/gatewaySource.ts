import {
	apiJobToJob,
	createRaijuClient,
	isNoSpace,
	isUnauthorized,
	isUnreachable,
	jobErrorText,
	type LiveFigures,
	pairingErrorText,
	probeSummary,
	RaijuApiError,
} from "@/api";
import type { ApiJob } from "@/api/types";
import {
	Aria2Client,
	type Aria2Snapshot,
	type Aria2Status,
	syncAria2,
} from "@/aria2";
import { aria2Files, aria2SourceName } from "./aria2Adapter";
import { fileAllocationFromOption } from "./fileAllocation";
import {
	type AddOptions,
	type Console,
	type Dashboard,
	type DashboardSource,
	type Device,
	destinations,
	type FolderRoot,
	type JobDetail,
	type JobStatus,
	type Settings,
} from "./types";

const historyLength = 60;

const pollIntervalMs = 1000;

const statusIntervalMs = 2000;

const storageIntervalMs = 10_000;

const sampledAtMs = 900;

export const defaultSettings: Settings = {
	speed: {
		profile: "balanced",
		connectionsPerServer: 16,
		split: 16,
		downloadLimit: 0,
		uploadLimit: 0,
	},
	downloads: {
		directory: destinations[0].path,
		concurrent: 5,
		fileAllocation: "none",
	},
	console: { keepAwake: true, notifications: true },
};

function noticeText(error: unknown): string {
	if (error instanceof RaijuApiError) {
		return error.code ? jobErrorText(error.code) : error.message;
	}
	return error instanceof Error ? error.message : String(error);
}

/** A job id is the aria2 GID, so it has to match the gateway's `[A-Za-z0-9_-]{1,64}`. */
function newJobId(): string {
	const bytes = crypto.getRandomValues(new Uint8Array(6));
	return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join(
		"",
	);
}

function numberOption(
	options: Record<string, string>,
	key: string,
	fallback: number,
): number {
	const value = Number(options[key]);
	return Number.isFinite(value) && value > 0 ? value : fallback;
}

function base64(buffer: ArrayBuffer): string {
	let binary = "";
	const bytes = new Uint8Array(buffer);
	for (let i = 0; i < bytes.length; i += 0x8000) {
		binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
	}
	return btoa(binary);
}

/**
 * Live source for a real console. Jobs, stages, storage, preset folders, the probe and pairing come
 * from the Raiju `/api`; speeds, connections, per-download options and the detail sheet come from
 * aria2 through the gateway's `/jsonrpc` proxy, because the API carries no engine figures.
 */
export function createGatewaySource(options: {
	baseUrl?: string;
	jsonrpcUrl: string;
}): DashboardSource {
	const client = createRaijuClient({ baseUrl: options.baseUrl });
	const engine = new Aria2Client({ url: options.jsonrpcUrl });
	const listeners = new Set<() => void>();
	const statuses = new Map<string, Aria2Status>();
	let settings: Settings = defaultSettings;
	let currentDeviceId: string | null = null;
	let state: Dashboard = {
		jobs: [],
		speedHistory: Array.from({ length: historyLength }, () => 0),
		storage: null,
		console: {
			host: window.location.hostname,
			online: false,
			version: "",
			engine: "",
			firmware: "",
			uptimeSeconds: null,
		},
		connection: "connecting",
		folders: destinations.map((root) => ({ ...root })),
		notice: null,
	};
	let stopped = true;
	let timer: ReturnType<typeof setTimeout> | null = null;
	let busy = false;
	let lastSample = 0;
	let lastStorage = 0;
	let foldersLoaded = false;
	let syncing = false;
	let stopSync: (() => void) | null = null;
	let refresh: () => Promise<void> = async () => undefined;

	function set(next: Dashboard): void {
		state = next;
		for (const listener of listeners) {
			listener();
		}
	}

	function onSnapshot(snapshot: Aria2Snapshot): void {
		statuses.clear();
		for (const status of [
			...snapshot.active,
			...snapshot.waiting,
			...snapshot.stopped,
		]) {
			statuses.set(status.gid, status);
		}
	}

	function startEngine(): void {
		if (syncing) {
			return;
		}
		syncing = true;
		engine.connect().catch(() => undefined);
		const sync = syncAria2(engine, onSnapshot, { onError: () => undefined });
		refresh = () => sync.refresh();
		stopSync = () => {
			sync.stop();
			engine.close();
		};
	}

	function stopEngine(): void {
		if (!syncing) {
			return;
		}
		syncing = false;
		stopSync?.();
		stopSync = null;
		statuses.clear();
		refresh = async () => undefined;
	}

	function liveStatus(job: ApiJob, status: Aria2Status): JobStatus | undefined {
		if (status.status === "paused") {
			return "paused";
		}
		if (
			status.status === "waiting" &&
			job.uiStage === "download" &&
			job.status === "active"
		) {
			return "waiting";
		}
		return undefined;
	}

	function liveFigures(job: ApiJob): LiveFigures {
		const status = statuses.get(job.id);
		if (!status) {
			return {};
		}
		const uris = [
			...new Set(
				(status.files ?? []).flatMap((file) => file.uris.map((uri) => uri.uri)),
			),
		];
		return {
			speed: Number(status.downloadSpeed),
			connections: Number(status.connections),
			status: liveStatus(job, status),
			uris,
		};
	}

	async function loadFolders(): Promise<FolderRoot[]> {
		try {
			const body = await client.folders();
			foldersLoaded = true;
			return body.roots.map((root) => ({ ...root }));
		} catch {
			return state.folders;
		}
	}

	async function applyJobs(apiJobs: ApiJob[]): Promise<void> {
		const jobs = apiJobs.map((job) => apiJobToJob(job, liveFigures(job)));
		const now = Date.now();
		const downloadSpeed = jobs
			.filter((job) => job.status === "active" && job.stage === "download")
			.reduce((sum, job) => sum + job.speed, 0);
		const speedHistory =
			now - lastSample >= sampledAtMs
				? [...state.speedHistory.slice(1), downloadSpeed]
				: state.speedHistory;
		if (speedHistory !== state.speedHistory) {
			lastSample = now;
		}
		let storage = state.storage;
		if (storage === null || now - lastStorage >= storageIntervalMs) {
			lastStorage = now;
			try {
				const figures = await client.storage();
				storage = {
					totalBytes: figures.totalBytes,
					freeBytes: figures.freeBytes,
					gamesBytes: figures.gamesBytes,
					stagingBytes: figures.stagingBytes,
				};
			} catch {
				storage = state.storage;
			}
		}
		const folders = foldersLoaded ? state.folders : await loadFolders();
		set({ ...state, jobs, speedHistory, storage, folders });
	}

	async function poll(): Promise<void> {
		let status: Awaited<ReturnType<typeof client.status>>;
		try {
			status = await client.status();
		} catch (error) {
			stopEngine();
			set({
				...state,
				connection: isUnauthorized(error)
					? "unpaired"
					: isUnreachable(error)
						? "unreachable"
						: state.connection,
				console: { ...state.console, online: false },
			});
			return;
		}
		const consoleInfo: Console = {
			host: status.ip || window.location.hostname,
			online: status.engine.up,
			version: status.version ? `raiju ${status.version}` : "",
			engine: status.engine.version ? `aria2 ${status.engine.version}` : "",
			firmware: status.firmware,
			uptimeSeconds: status.uptime,
		};
		let apiJobs: ApiJob[];
		try {
			apiJobs = await client.jobs();
		} catch (error) {
			stopEngine();
			if (isUnauthorized(error)) {
				set({ ...state, connection: "unpaired", console: consoleInfo });
				return;
			}
			if (isUnreachable(error)) {
				set({
					...state,
					connection: "unreachable",
					console: { ...consoleInfo, online: false },
					notice: noticeText(error),
				});
				return;
			}
			set({
				...state,
				connection: "online",
				console: consoleInfo,
				notice: noticeText(error),
			});
			return;
		}
		if (state.connection !== "online") {
			startEngine();
		}
		set({ ...state, connection: "online", console: consoleInfo });
		await applyJobs(apiJobs);
	}

	function schedule(delay: number): void {
		if (stopped || timer !== null) {
			return;
		}
		timer = setTimeout(() => void tick(), delay);
	}

	async function tick(): Promise<void> {
		timer = null;
		if (stopped || busy) {
			return;
		}
		if (document.visibilityState === "hidden") {
			schedule(statusIntervalMs);
			return;
		}
		busy = true;
		try {
			await poll();
		} catch (error) {
			set({ ...state, notice: noticeText(error) });
		} finally {
			busy = false;
			schedule(
				state.connection === "online" ? pollIntervalMs : statusIntervalMs,
			);
		}
	}

	function run(action: () => Promise<unknown>): void {
		action()
			.then(() => refresh())
			.catch((error) => set({ ...state, notice: noticeText(error) }));
	}

	function actionError(error: unknown): string {
		return error instanceof RaijuApiError && error.status === 409
			? jobErrorText(error.code)
			: noticeText(error);
	}

	/**
	 * Adds a download the pipeline owns: a URL, or the base64 contents of a `.torrent`. Either way the
	 * job carries the id aria2's notifications come back with, plus the same destination, title ID,
	 * archive and conflict options.
	 */
	async function addJob(
		body: { uri: string } | { torrent: string },
		options: AddOptions,
	): Promise<void> {
		try {
			await client.addJob({
				id: newJobId(),
				...body,
				destination: options.destination,
				titleId: options.titleId,
				keepArchive: options.keepArchive,
				conflict: options.conflict,
			});
		} catch (error) {
			if (isNoSpace(error)) {
				throw new Error(jobErrorText("no_space"));
			}
			throw error;
		}
	}

	/**
	 * Removing is the endpoint's job: it deletes the job's staging and trash folders and, unless it
	 * already installed, its download. A job the processor is still working on is refused, so it is
	 * cancelled first, which also stops a stage in flight. The engine cleanup behind that is best
	 * effort, because a job that never reached the engine is normal.
	 */
	async function removeJob(id: string): Promise<void> {
		try {
			await client.removeJob(id);
		} catch (error) {
			const refused = error instanceof RaijuApiError && error.status === 409;
			const alreadyGone =
				error instanceof RaijuApiError && error.status === 404;
			if (refused) {
				await client.cancelJob(id).catch(() => undefined);
				try {
					await client.removeJob(id);
				} catch (retryError) {
					set({ ...state, notice: actionError(retryError) });
					return;
				}
			} else if (!alreadyGone) {
				set({ ...state, notice: actionError(error) });
				return;
			}
		}
		await engine.remove(id, true).catch(() => undefined);
		await engine.removeDownloadResult(id).catch(() => undefined);
		await refresh();
	}

	async function loadDetail(id: string): Promise<JobDetail | null> {
		const job = state.jobs.find((item) => item.id === id);
		if (!job) {
			return null;
		}
		try {
			const status = await engine.tellStatus(id);
			const options = await engine
				.getOption(id)
				.catch(() => ({}) as Record<string, string>);
			const isTorrent = Boolean(status.bittorrent);
			let connections: JobDetail["connections"] = [];
			if (status.status === "active") {
				connections = isTorrent
					? (await engine.getPeers(id)).map((peer) => ({
							label: `${peer.ip}:${peer.port}`,
							detail: peer.seeder === "true" ? "seeder" : "peer",
							downloadSpeed: Number(peer.downloadSpeed),
							uploadSpeed: Number(peer.uploadSpeed),
						}))
					: (await engine.getServers(id)).flatMap((file) =>
							file.servers.map((server) => ({
								label: new URL(server.currentUri).host,
								detail: server.currentUri,
								downloadSpeed: Number(server.downloadSpeed),
								uploadSpeed: 0,
							})),
						);
			}
			return {
				job: { ...job, name: aria2SourceName(status) || job.name },
				directory: status.dir,
				files: aria2Files(status),
				connections,
				connectionKind: isTorrent ? "peers" : "servers",
				maxDownloadLimit: Number(options["max-download-limit"] ?? 0),
			};
		} catch (error) {
			set({ ...state, notice: noticeText(error) });
			return null;
		}
	}

	async function loadSettings(): Promise<Settings> {
		try {
			const options = (await engine.getGlobalOption()) as Record<
				string,
				string
			>;
			settings = {
				speed: {
					...settings.speed,
					connectionsPerServer: numberOption(
						options,
						"max-connection-per-server",
						settings.speed.connectionsPerServer,
					),
					split: numberOption(options, "split", settings.speed.split),
					downloadLimit: Number(options["max-overall-download-limit"] ?? 0),
					uploadLimit: Number(options["max-overall-upload-limit"] ?? 0),
				},
				downloads: {
					...settings.downloads,
					concurrent: numberOption(
						options,
						"max-concurrent-downloads",
						settings.downloads.concurrent,
					),
					fileAllocation: fileAllocationFromOption(options["file-allocation"]),
				},
				console: settings.console,
			};
		} catch {
			return settings;
		}
		return settings;
	}

	/**
	 * Applies the settings to the engine and to the payload's power options, remembering each step as
	 * it succeeds, so a refused switch never makes the UI claim a value the console didn't take.
	 */
	async function saveSettings(next: Settings): Promise<boolean> {
		try {
			await engine.changeGlobalOption({
				"max-connection-per-server": String(next.speed.connectionsPerServer),
				split: String(next.speed.split),
				"max-overall-download-limit": String(next.speed.downloadLimit),
				"max-overall-upload-limit": String(next.speed.uploadLimit),
				"max-concurrent-downloads": String(next.downloads.concurrent),
				"file-allocation": next.downloads.fileAllocation,
			});
		} catch (error) {
			set({ ...state, notice: noticeText(error) });
			return false;
		}
		settings = { ...settings, speed: next.speed, downloads: next.downloads };
		const switches = [
			{
				name: "keep awake",
				endpoint: "keep-awake" as const,
				key: "keepAwake" as const,
				on: next.console.keepAwake,
				before: settings.console.keepAwake,
			},
			{
				name: "notifications",
				endpoint: "notifications" as const,
				key: "notifications" as const,
				on: next.console.notifications,
				before: settings.console.notifications,
			},
		];
		for (const item of switches) {
			if (item.on === item.before) {
				continue;
			}
			try {
				await client.power(item.endpoint, item.on);
			} catch (error) {
				set({
					...state,
					notice:
						error instanceof RaijuApiError && error.status === 404
							? `Changing ${item.name} isn't available in this payload version.`
							: noticeText(error),
				});
				return false;
			}
			settings = {
				...settings,
				console:
					item.key === "keepAwake"
						? { ...settings.console, keepAwake: item.on }
						: { ...settings.console, notifications: item.on },
			};
		}
		return true;
	}

	return {
		mode: "live",
		getState: () => state,
		subscribe: (listener) => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		start: () => {
			if (!stopped) {
				return;
			}
			stopped = false;
			schedule(0);
		},
		stop: () => {
			stopped = true;
			if (timer !== null) {
				clearTimeout(timer);
				timer = null;
			}
			stopEngine();
		},
		actions: {
			togglePause: (id) => {
				const status = statuses.get(id);
				if (!status) {
					set({
						...state,
						notice: "This download isn't in the engine right now.",
					});
					return;
				}
				run(() =>
					status.status === "paused" ? engine.unpause(id) : engine.pause(id),
				);
			},
			pauseAll: () => run(() => engine.pauseAll()),
			resumeAll: () => run(() => engine.unpauseAll()),
			retry: (id) => {
				client
					.retryJob(id)
					.then(() => refresh())
					.catch((error) => set({ ...state, notice: actionError(error) }));
			},
			remove: (id) => void removeJob(id),
			addLinks: (links, options) => {
				Promise.all(links.map((link) => addJob({ uri: link }, options)))
					.then(() => refresh())
					.catch((error) => set({ ...state, notice: actionError(error) }));
			},
			addTorrents: (files, options) => {
				Promise.all(
					files.map(async (file) =>
						addJob({ torrent: base64(await file.arrayBuffer()) }, options),
					),
				)
					.then(() => refresh())
					.catch((error) => set({ ...state, notice: noticeText(error) }));
			},
			setSpeedLimit: (id, bytesPerSecond) => {
				run(() =>
					engine.changeOption(id, {
						"max-download-limit": String(
							Math.max(0, Math.round(bytesPerSecond)),
						),
					}),
				);
			},
			loadDetail,
			resolveConflict: (id, choice) => {
				client
					.resolveJob(id, choice)
					.then(() => refresh())
					.catch((error) => set({ ...state, notice: actionError(error) }));
			},
			resolveTitle: (id, titleId) => {
				client
					.titleJob(id, titleId)
					.then(() => refresh())
					.catch((error) => set({ ...state, notice: actionError(error) }));
			},
			probe: async (uri) => {
				try {
					return probeSummary(await client.probe(uri));
				} catch (error) {
					if (!isUnreachable(error)) {
						set({ ...state, notice: noticeText(error) });
					}
					return null;
				}
			},
			pair: async (code) => {
				try {
					const body = await client.pair(code, "browser");
					currentDeviceId = body.id;
					await poll();
					return true;
				} catch (error) {
					set({
						...state,
						notice: pairingErrorText(
							error instanceof RaijuApiError ? error.code : noticeText(error),
						),
					});
					return false;
				}
			},
			loadDevices: async () => {
				try {
					const body = await client.devices();
					return body.map(
						(device): Device => ({
							id: device.id,
							label: device.label,
							current: device.id === currentDeviceId,
						}),
					);
				} catch (error) {
					set({ ...state, notice: noticeText(error) });
					return [];
				}
			},
			revokeDevice: async (id) => {
				try {
					await client.revokeDevice(id);
				} catch (error) {
					set({ ...state, notice: noticeText(error) });
				}
			},
			loadSettings,
			saveSettings,
			dismissNotice: () => set({ ...state, notice: null }),
		},
	};
}
