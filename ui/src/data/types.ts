export type Stage = "download" | "verify" | "extract" | "install" | "ready";

export type JobKind = "image" | "archive" | "folder";

export type JobStatus = "active" | "paused" | "waiting" | "done" | "failed";

export type Job = {
	id: string;
	titleId: string;
	name: string;
	sourceName: string;
	kind: JobKind;
	format: string;
	installName: string;
	stage: Stage;
	status: JobStatus;
	totalBytes: number;
	stageBytes: number;
	stageDoneBytes: number;
	speed: number;
	connections: number;
	error?: string;
	finishedAt?: number;
};

export type Storage = {
	totalBytes: number;
	gamesBytes: number;
	stagingBytes: number;
};

export type Console = {
	host: string;
	online: boolean;
	version: string;
	uptimeSeconds: number;
};

export type Dashboard = {
	jobs: Job[];
	speedHistory: number[];
	storage: Storage;
	console: Console;
};

export const stagesFor: Record<JobKind, Stage[]> = {
	image: ["download", "verify", "install", "ready"],
	archive: ["download", "verify", "extract", "install", "ready"],
	folder: ["download", "verify", "extract", "install", "ready"],
};
