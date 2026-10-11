/** Raiju API vocabulary, mirroring the gateway handlers in `payload/gateway/src/api.c`. */

/** Engine block of `GET /api/status`. `version` is absent while the engine is down. */
export type ApiEngine = {
	up: boolean;
	version?: string;
};

export type ApiStatus = {
	name: string;
	version: string;
	uptime: number;
	firmware: string;
	platform: "host" | "ps5";
	ip: string;
	engine: ApiEngine;
};

export type ApiStorage = {
	totalBytes: number;
	freeBytes: number;
	gamesBytes: number;
	stagingBytes: number;
};

export type ApiRoot = {
	id: string;
	label: string;
	path: string;
};

export type ApiEntry = {
	name: string;
	path: string;
	directory: boolean;
};

export type ApiFolders = {
	roots: ApiRoot[];
	path: string;
	entries: ApiEntry[];
};

/** Raw processor stage, e.g. `awaiting_title_id` or `conflict`. */
export type ApiRawStage =
	| "downloading"
	| "verifying"
	| "classifying"
	| "extracting"
	| "installing"
	| "rescan"
	| "notifying"
	| "ready"
	| "awaiting_title_id"
	| "conflict"
	| "failed"
	| "cancelled";

export type ApiUiStage =
	| "download"
	| "verify"
	| "extract"
	| "install"
	| "ready";

/** `waiting` covers both `awaiting_title_id` and `conflict`; `uiStage` says which. */
export type ApiUiStatus = "active" | "waiting" | "done" | "failed";

export type ApiKind =
	| "unknown"
	| "7z"
	| "zip"
	| "rar"
	| "zstd"
	| "xz"
	| "gzip"
	| "exfat"
	| "ufs2"
	| "pfs"
	| "pfsc";

export type ApiJob = {
	id: string;
	stage: ApiRawStage;
	uiStage: ApiUiStage;
	status: ApiUiStatus;
	titleId: string;
	sourceName: string;
	name: string;
	kind: ApiKind;
	installName: string;
	bytesTotal: number;
	bytesDone: number;
	error: string;
};

export type ApiJobs = {
	jobs: ApiJob[];
};

/** `POST /api/probe` verdict. `uncompressedSize` is only meaningful when `uncompressedKnown`. */
export type ApiProbe = {
	size: number;
	kind: ApiKind;
	category: "archive" | "image" | "other";
	uncompressedSize: number;
	uncompressedKnown: boolean;
	estimated: boolean;
	fits: boolean;
	requiredBytes: number;
	freeBytes: number;
};

/** A 409 from `POST /api/jobs`: the archive and its contents would not fit. */
export type ApiNoSpace = {
	error: "no_space";
	requiredBytes: number;
	freeBytes: number;
	estimated: boolean;
};

export type ApiDevice = {
	id: string;
	label: string;
};

export type ApiDevices = {
	devices: ApiDevice[];
};

/** `POST /api/pair` response. The token is also set as the `raiju_device` cookie. */
export type ApiPairing = {
	id: string;
	token: string;
	label: string;
};

export type ConflictChoice = "rename" | "replace" | "cancel";

/** `POST /api/jobs` body. `id` is the aria2 GID, so notifications map back to the job. */
export type ApiNewJob = {
	id: string;
	uri: string;
	destination?: string;
	titleId?: string;
	fileName?: string;
	kind?: ApiKind;
	size?: number;
	uncompressedSize?: number;
	keepArchive?: boolean;
	conflict?: "rename" | "replace";
};
