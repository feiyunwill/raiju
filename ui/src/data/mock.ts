import { type Dashboard, type Job, stagesFor } from "./types";

const GB = 1_000_000_000;

const baseSpeed = 96_000_000;

const extractSpeed = 140_000_000;

const settleBytesPerTick = 4 * GB;

/** Builds the initial demo state shown until the UI is wired to a real console. */
export function createMockDashboard(): Dashboard {
	const now = Date.now();
	const jobs: Job[] = [
		{
			id: "a1",
			titleId: "PPSA25380",
			name: "Elden Ring Nightreign",
			sourceName: "PPSA25380-elden-ring-nightreign.7z",
			kind: "archive",
			format: "7z",
			installName: "PPSA25380.exfat",
			stage: "download",
			status: "active",
			totalBytes: 68.4 * GB,
			stageBytes: 68.4 * GB,
			stageDoneBytes: 29.2 * GB,
			speed: baseSpeed,
			connections: 16,
		},
		{
			id: "b2",
			titleId: "PPSA10872",
			name: "Hollow Peaks",
			sourceName: "PPSA10872.ffpkg",
			kind: "image",
			format: "ffpkg",
			installName: "PPSA10872.ffpkg",
			stage: "download",
			status: "waiting",
			totalBytes: 41.7 * GB,
			stageBytes: 41.7 * GB,
			stageDoneBytes: 0,
			speed: 0,
			connections: 0,
		},
		{
			id: "c3",
			titleId: "CUSA31544",
			name: "Starfall Tactics",
			sourceName: "starfall-tactics-CUSA31544.zip",
			kind: "folder",
			format: "zip",
			installName: "CUSA31544",
			stage: "download",
			status: "paused",
			totalBytes: 23.1 * GB,
			stageBytes: 23.1 * GB,
			stageDoneBytes: 6.4 * GB,
			speed: 0,
			connections: 0,
		},
		{
			id: "d4",
			titleId: "CUSA00003",
			name: "Driveclub",
			sourceName: "CUSA00003.exfat",
			kind: "image",
			format: "exfat",
			installName: "CUSA00003.exfat",
			stage: "ready",
			status: "done",
			totalBytes: 52.9 * GB,
			stageBytes: 52.9 * GB,
			stageDoneBytes: 52.9 * GB,
			speed: 0,
			connections: 0,
			finishedAt: now - 42 * 60_000,
		},
		{
			id: "e5",
			titleId: "PPSA18230",
			name: "Tidebound",
			sourceName: "tidebound.ffpkg",
			kind: "image",
			format: "ffpkg",
			installName: "PPSA18230.ffpkg",
			stage: "verify",
			status: "failed",
			totalBytes: 17.6 * GB,
			stageBytes: 17.6 * GB,
			stageDoneBytes: 17.6 * GB,
			speed: 0,
			connections: 0,
			error:
				"Image header is not UFS2. The server may have sent an error page.",
		},
	];
	const speedHistory = Array.from(
		{ length: 60 },
		(_, i) =>
			baseSpeed * (0.82 + 0.12 * Math.sin(i / 5) + Math.random() * 0.08),
	);
	return {
		jobs,
		speedHistory,
		storage: {
			totalBytes: 825 * GB,
			gamesBytes: 486 * GB,
			stagingBytes: 29.2 * GB,
		},
		console: {
			host: "192.168.1.50",
			online: true,
			version: "0.1.0",
			uptimeSeconds: 3 * 3600 + 12 * 60,
		},
		notice: null,
	};
}

function nextStage(job: Job): Job {
	const stages = stagesFor[job.kind];
	const next = stages[stages.indexOf(job.stage) + 1] ?? "ready";
	if (next === "ready") {
		return {
			...job,
			stage: "ready",
			status: "done",
			speed: 0,
			connections: 0,
			finishedAt: Date.now(),
		};
	}
	const stageBytes =
		next === "extract" ? job.totalBytes * 1.06 : job.totalBytes;
	return { ...job, stage: next, stageBytes, stageDoneBytes: 0, connections: 0 };
}

function advance(job: Job): Job {
	if (job.status !== "active") {
		return job;
	}
	const rate =
		job.stage === "download"
			? baseSpeed * (0.85 + Math.random() * 0.25)
			: job.stage === "extract"
				? extractSpeed * (0.9 + Math.random() * 0.2)
				: settleBytesPerTick;
	const stageDoneBytes = Math.min(job.stageBytes, job.stageDoneBytes + rate);
	const updated: Job = {
		...job,
		stageDoneBytes,
		speed: job.stage === "download" || job.stage === "extract" ? rate : 0,
		connections: job.stage === "download" ? 16 : 0,
	};
	return stageDoneBytes >= job.stageBytes ? nextStage(updated) : updated;
}

/** Advances the demo by one second: active jobs progress and the next waiting job starts when none is active. */
export function tickMockDashboard(state: Dashboard): Dashboard {
	let jobs = state.jobs.map(advance);
	if (!jobs.some((job) => job.status === "active")) {
		const waiting = jobs.findIndex((job) => job.status === "waiting");
		if (waiting >= 0) {
			jobs = jobs.map((job, i) =>
				i === waiting ? { ...job, status: "active" } : job,
			);
		}
	}
	const downloadSpeed = jobs
		.filter((job) => job.status === "active" && job.stage === "download")
		.reduce((sum, job) => sum + job.speed, 0);
	const downloadedThisTick = state.jobs
		.filter((job) => job.status === "active" && job.stage === "download")
		.reduce((sum, job) => sum + job.speed, 0);
	return {
		...state,
		jobs,
		speedHistory: [...state.speedHistory.slice(1), downloadSpeed],
		storage: state.storage && {
			...state.storage,
			stagingBytes: state.storage.stagingBytes + downloadedThisTick,
		},
		console: {
			...state.console,
			uptimeSeconds: (state.console.uptimeSeconds ?? 0) + 1,
		},
	};
}
