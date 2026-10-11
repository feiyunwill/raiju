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
	uris?: string[];
};

export type JobFile = {
	index: number;
	path: string;
	length: number;
	completed: number;
	selected: boolean;
};

export type JobConnection = {
	label: string;
	detail: string;
	downloadSpeed: number;
	uploadSpeed: number;
};

export type JobDetail = {
	job: Job;
	directory: string;
	files: JobFile[];
	connections: JobConnection[];
	connectionKind: "servers" | "peers";
	maxDownloadLimit: number;
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
	uptimeSeconds: number | null;
};

export type Dashboard = {
	jobs: Job[];
	speedHistory: number[];
	storage: Storage | null;
	console: Console;
	notice: string | null;
};

export type AddOptions = {
	destination: string;
};

export type DashboardActions = {
	togglePause: (id: string) => void;
	pauseAll: () => void;
	resumeAll: () => void;
	retry: (id: string) => void;
	remove: (id: string) => void;
	addLinks: (links: string[], options: AddOptions) => void;
	addTorrents: (files: File[], options: AddOptions) => void;
	setSpeedLimit: (id: string, bytesPerSecond: number) => void;
	loadDetail: (id: string) => Promise<JobDetail | null>;
	dismissNotice: () => void;
};

export type SourceMode = "demo" | "live";

export type DashboardSource = {
	mode: SourceMode;
	getState: () => Dashboard;
	subscribe: (listener: () => void) => () => void;
	start: () => void;
	stop: () => void;
	actions: DashboardActions;
};

export const destinations = [
	{ id: "homebrew", label: "homebrew", path: "/data/homebrew" },
	{ id: "etahen", label: "etaHEN/games", path: "/data/etaHEN/games" },
	{ id: "downloads", label: "Downloads", path: "/data/raiju/downloads" },
] as const;

export const stagesFor: Record<JobKind, Stage[]> = {
	image: ["download", "verify", "install", "ready"],
	archive: ["download", "verify", "extract", "install", "ready"],
	folder: ["download", "verify", "extract", "install", "ready"],
};
