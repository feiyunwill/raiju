import { describeSource } from "./describe";
import { createMockDashboard, tickMockDashboard } from "./mock";
import type {
	Dashboard,
	DashboardSource,
	Job,
	JobDetail,
	JobStatus,
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
			dismissNotice: () => set({ ...state, notice: null }),
		},
	};
}
