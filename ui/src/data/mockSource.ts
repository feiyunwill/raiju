import { kindBadge } from "@/api";
import { formatBytes } from "@/lib/format";
import { describeSource } from "./describe";
import { defaultSettings } from "./gatewaySource";
import { createMockDashboard, tickMockDashboard } from "./mock";
import type {
	Dashboard,
	DashboardSource,
	Device,
	Job,
	JobDetail,
	JobStatus,
	ProbeVerdict,
	Settings,
} from "./types";

/** Builds a waiting demo job from a pasted link. */
export function jobFromLink(link: string, index: number): Job {
	const description = describeSource(
		link.split(/[?#]/)[0]?.split("/").pop() || link,
	);
	const totalBytes = (18 + ((index * 7919) % 50)) * 1_000_000_000;
	return {
		id: `${Date.now().toString(36)}-${index}`,
		...description,
		stage: "download",
		status: "waiting",
		totalBytes,
		stageBytes: totalBytes,
		stageDoneBytes: 0,
		speed: 0,
		connections: 0,
		uris: [link],
	};
}

function hashOf(text: string): number {
	let hash = 0;
	for (let i = 0; i < text.length; i++) {
		hash = (hash * 31 + text.charCodeAt(i)) % 1_000_000_007;
	}
	return hash;
}

/** Stands in for the probe: the same wording, from a size derived from the link itself. */
function demoVerdict(link: string, freeBytes: number): ProbeVerdict {
	const description = describeSource(
		link.split(/[?#]/)[0]?.split("/").pop() || link,
	);
	const size = (20 + (hashOf(link) % 60)) * 1_000_000_000;
	const uncompressed = Math.round(size * 1.06);
	const required = uncompressed + 1_000_000_000;
	const fits = required <= freeBytes;
	return {
		badge: kindBadge(description.format),
		size: formatBytes(size),
		extracted: `→ ${formatBytes(uncompressed)}`,
		required: `needs ${formatBytes(required)}`,
		verdict: fits
			? `fits (${formatBytes(freeBytes)} free)`
			: `needs ${formatBytes(required - freeBytes)} more`,
		destructive: !fits,
		category: description.kind === "image" ? "image" : "archive",
	};
}

function demoDetail(job: Job): JobDetail {
	const connections = Array.from({ length: job.connections }, (_, i) => ({
		label: `cdn-${(i % 4) + 1}.example.com`,
		detail: `connection ${i + 1}`,
		downloadSpeed: job.connections ? job.speed / job.connections : 0,
		uploadSpeed: 0,
	}));
	return {
		job,
		directory: "/data/raiju/downloads",
		files: [
			{
				index: 1,
				path: `/data/raiju/downloads/${job.sourceName}`,
				length: job.totalBytes,
				completed:
					job.stage === "download" ? job.stageDoneBytes : job.totalBytes,
				selected: true,
			},
		],
		connections,
		connectionKind: "servers",
		maxDownloadLimit: 0,
	};
}

/** Demo source: a one-second simulation used when no console is configured. */
export function createMockSource(): DashboardSource {
	let state: Dashboard = createMockDashboard();
	let timer: ReturnType<typeof setInterval> | null = null;
	const listeners = new Set<() => void>();
	const limits = new Map<string, number>();
	let settings: Settings = defaultSettings;
	let devices: Device[] = [
		{ id: "demo-laptop", label: "This browser", current: true },
		{ id: "demo-tablet", label: "iPad", current: false },
	];

	const set = (next: Dashboard) => {
		state = next;
		for (const listener of listeners) {
			listener();
		}
	};

	const updateJobs = (update: (job: Job) => Job) =>
		set({ ...state, jobs: state.jobs.map(update) });

	const setStatuses = (from: JobStatus[], to: JobStatus) =>
		updateJobs((job) =>
			from.includes(job.status)
				? { ...job, status: to, speed: 0, connections: 0 }
				: job,
		);

	return {
		mode: "demo",
		getState: () => state,
		subscribe: (listener) => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		start: () => {
			if (timer === null) {
				timer = setInterval(() => {
					if (document.visibilityState === "visible") {
						set(tickMockDashboard(state));
					}
				}, 1000);
			}
		},
		stop: () => {
			if (timer !== null) {
				clearInterval(timer);
				timer = null;
			}
		},
		actions: {
			togglePause: (id) =>
				updateJobs((job) => {
					if (job.id !== id) {
						return job;
					}
					if (job.status === "active") {
						return { ...job, status: "paused", speed: 0, connections: 0 };
					}
					return job.status === "paused" || job.status === "waiting"
						? { ...job, status: "active" }
						: job;
				}),
			pauseAll: () => setStatuses(["active", "waiting"], "paused"),
			resumeAll: () => setStatuses(["paused"], "active"),
			retry: (id) =>
				updateJobs((job) =>
					job.id === id
						? {
								...job,
								status: "waiting",
								stage: "download",
								stageDoneBytes: 0,
								error: undefined,
							}
						: job,
				),
			remove: (id) =>
				set({ ...state, jobs: state.jobs.filter((job) => job.id !== id) }),
			addLinks: (links) =>
				set({ ...state, jobs: [...state.jobs, ...links.map(jobFromLink)] }),
			addTorrents: (files) =>
				set({
					...state,
					jobs: [
						...state.jobs,
						...files.map((file, i) =>
							jobFromLink(file.name, i + state.jobs.length),
						),
					],
				}),
			setSpeedLimit: (id, bytesPerSecond) => {
				limits.set(id, bytesPerSecond);
			},
			loadDetail: async (id) => {
				const job = state.jobs.find((item) => item.id === id);
				return job
					? { ...demoDetail(job), maxDownloadLimit: limits.get(id) ?? 0 }
					: null;
			},
			resolveConflict: (id, choice) =>
				set({
					...state,
					jobs:
						choice === "cancel"
							? state.jobs.filter((job) => job.id !== id)
							: state.jobs.map((job) =>
									job.id === id
										? {
												...job,
												status: "active",
												stageDoneBytes: 0,
												installName:
													choice === "rename"
														? job.installName.replace(
																/(\.[^.]+)?$/,
																(extension) => `-2${extension}`,
															)
														: job.installName,
											}
										: job,
								),
				}),
			resolveTitle: (id, titleId) =>
				set({
					...state,
					jobs: state.jobs.map((job) =>
						job.id === id
							? { ...job, status: "active", stage: "install", titleId }
							: job,
					),
				}),
			probe: async (uri) => demoVerdict(uri, state.storage?.freeBytes ?? 0),
			pair: async (code) => {
				if (!/^\d{4}$/.test(code)) {
					set({
						...state,
						notice: "Enter the 4-digit code shown on your PS5.",
					});
					return false;
				}
				set({ ...state, connection: "online", notice: null });
				return true;
			},
			loadDevices: async () => devices,
			revokeDevice: async (id) => {
				devices = devices.filter((device) => device.id !== id);
			},
			loadSettings: async () => settings,
			saveSettings: async (next) => {
				settings = next;
				return true;
			},
			dismissNotice: () => set({ ...state, notice: null }),
		},
	};
}
