import type { ConflictChoice } from "@/api/types";

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
	freeBytes: number;
	gamesBytes: number;
	stagingBytes: number;
};

export type Console = {
	host: string;
	online: boolean;
	version: string;
	engine: string;
	firmware: string;
	uptimeSeconds: number | null;
};

/** How the UI is talking to the console right now, and what to show when it isn't. */
export type ConnectionState =
	| "connecting"
	| "online"
	| "unpaired"
	| "unreachable";

export type FolderRoot = {
	id: string;
	label: string;
	path: string;
};

export type Dashboard = {
	jobs: Job[];
	speedHistory: number[];
	storage: Storage | null;
	console: Console;
	connection: ConnectionState;
	folders: FolderRoot[];
	notice: string | null;
};

export type AddOptions = {
	destination: string;
	titleId?: string;
	keepArchive?: boolean;
	conflict?: "rename" | "replace";
};

/** The probe verdict as the Add-download row shows it, e.g. `ARCHIVE · 7z · 38 GB → 41 GB`. */
export type ProbeVerdict = {
	badge: string;
	size: string;
	extracted: string;
	required: string;
	verdict: string;
	destructive: boolean;
	category: "archive" | "image" | "other";
};

export type Device = {
	id: string;
	label: string;
	current: boolean;
};

export type SpeedProfile = "balanced" | "max" | "custom";

export type SpeedSettings = {
	profile: SpeedProfile;
	connectionsPerServer: number;
	split: number;
	downloadLimit: number;
	uploadLimit: number;
};

export type FileAllocation = "none" | "trunc";

export type DownloadSettings = {
	directory: string;
	concurrent: number;
	fileAllocation: FileAllocation;
};

export type ConsoleSettings = {
	keepAwake: boolean;
	notifications: boolean;
};

export type Settings = {
	speed: SpeedSettings;
	downloads: DownloadSettings;
	console: ConsoleSettings;
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
	/** Answers a name-conflict prompt and lets the job carry on. */
	resolveConflict: (id: string, choice: ConflictChoice) => void;
	/** Names a job whose dump carried no title ID, so it can install. */
	resolveTitle: (id: string, titleId: string) => void;
	/** One URL at a time; null while the verdict is unknown. */
	probe: (uri: string) => Promise<ProbeVerdict | null>;
	/** Exchanges the code shown on the PS5 for a device token. True once paired. */
	pair: (code: string) => Promise<boolean>;
	loadDevices: () => Promise<Device[]>;
	revokeDevice: (id: string) => Promise<void>;
	loadSettings: () => Promise<Settings>;
	/** True when the console accepted the change; false when it refused it. */
	saveSettings: (next: Settings) => Promise<boolean>;
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
