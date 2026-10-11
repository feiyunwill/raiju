export type Aria2Gid = string;

export type Aria2DownloadStatus =
	| "active"
	| "waiting"
	| "paused"
	| "error"
	| "complete"
	| "removed";

export type Aria2Uri = {
	uri: string;
	status: "used" | "waiting";
};

export type Aria2File = {
	index: string;
	path: string;
	length: string;
	completedLength: string;
	selected: "true" | "false";
	uris: Aria2Uri[];
};

export type Aria2Status = {
	gid: Aria2Gid;
	status: Aria2DownloadStatus;
	totalLength: string;
	completedLength: string;
	uploadLength: string;
	downloadSpeed: string;
	uploadSpeed: string;
	connections: string;
	numSeeders?: string;
	seeder?: "true" | "false";
	pieceLength?: string;
	numPieces?: string;
	errorCode?: string;
	errorMessage?: string;
	followedBy?: Aria2Gid[];
	following?: Aria2Gid;
	belongsTo?: Aria2Gid;
	dir: string;
	files: Aria2File[];
	infoHash?: string;
	bittorrent?: {
		announceList?: string[][];
		comment?: string;
		creationDate?: number;
		mode?: "single" | "multi";
		info?: { name: string };
	};
};

export type Aria2StatusKey = keyof Aria2Status;

export type Aria2GlobalStat = {
	downloadSpeed: string;
	uploadSpeed: string;
	numActive: string;
	numWaiting: string;
	numStopped: string;
	numStoppedTotal: string;
};

export type Aria2Version = {
	version: string;
	enabledFeatures: string[];
};

export type Aria2Options = Record<string, string>;

export type Aria2Peer = {
	peerId: string;
	ip: string;
	port: string;
	bitfield: string;
	amChoking: "true" | "false";
	peerChoking: "true" | "false";
	downloadSpeed: string;
	uploadSpeed: string;
	seeder: "true" | "false";
};

export type Aria2Server = {
	uri: string;
	currentUri: string;
	downloadSpeed: string;
};

export type Aria2FileServers = {
	index: string;
	servers: Aria2Server[];
};

export type Aria2Event =
	| "aria2.onDownloadStart"
	| "aria2.onDownloadPause"
	| "aria2.onDownloadStop"
	| "aria2.onDownloadComplete"
	| "aria2.onDownloadError"
	| "aria2.onBtDownloadComplete";

export type Aria2EventHandler = (gid: Aria2Gid, event: Aria2Event) => void;

export class Aria2RpcError extends Error {
	readonly code: number;

	constructor(code: number, message: string) {
		super(message);
		this.name = "Aria2RpcError";
		this.code = code;
	}
}
