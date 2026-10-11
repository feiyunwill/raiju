import {
	type Aria2Event,
	type Aria2EventHandler,
	type Aria2Gid,
	type Aria2GlobalStat,
	type Aria2Options,
	Aria2RpcError,
	type Aria2Status,
	type Aria2StatusKey,
	type Aria2Version,
} from "./types";

type Pending = {
	resolve: (value: unknown) => void;
	reject: (reason: unknown) => void;
	timer: ReturnType<typeof setTimeout>;
};

type RpcMessage = {
	id?: string;
	method?: string;
	params?: unknown;
	result?: unknown;
	error?: { code: number; message: string };
};

export type Aria2ClientOptions = {
	url: string;
	secret?: string;
	timeoutMs?: number;
	reconnect?: boolean;
	WebSocket?: typeof WebSocket;
	fetch?: typeof fetch;
};

const events: Aria2Event[] = [
	"aria2.onDownloadStart",
	"aria2.onDownloadPause",
	"aria2.onDownloadStop",
	"aria2.onDownloadComplete",
	"aria2.onDownloadError",
	"aria2.onBtDownloadComplete",
];

/** Converts an `http(s)://…/jsonrpc` endpoint to its `ws(s)://` twin. */
export function toWebSocketUrl(url: string): string {
	return url.replace(/^http/, "ws");
}

/** Converts a `ws(s)://…/jsonrpc` endpoint to its `http(s)://` twin. */
export function toHttpUrl(url: string): string {
	return url.replace(/^ws/, "http");
}

function eventGids(params: unknown): Aria2Gid[] {
	const list = Array.isArray(params) ? params : [params];
	return list
		.map((item) =>
			item && typeof item === "object" && "gid" in item
				? String((item as { gid: unknown }).gid)
				: null,
		)
		.filter((gid): gid is Aria2Gid => gid !== null);
}

/**
 * Typed aria2 JSON-RPC client. Calls go over a WebSocket when one is open and fall back to HTTP POST
 * otherwise; download notifications arrive over the WebSocket, which reconnects with backoff.
 */
export class Aria2Client {
	private readonly httpUrl: string;
	private readonly wsUrl: string;
	private readonly secret: string | undefined;
	private readonly timeoutMs: number;
	private readonly reconnect: boolean;
	private readonly WebSocketImpl: typeof WebSocket | undefined;
	private readonly fetchImpl: typeof fetch;
	private readonly pending = new Map<string, Pending>();
	private readonly handlers = new Set<Aria2EventHandler>();
	private readonly connectionListeners = new Set<(open: boolean) => void>();
	private socket: WebSocket | null = null;
	private nextId = 0;
	private retryDelay = 500;
	private retryTimer: ReturnType<typeof setTimeout> | null = null;
	private closed = false;

	constructor(options: Aria2ClientOptions) {
		this.httpUrl = toHttpUrl(options.url);
		this.wsUrl = toWebSocketUrl(options.url);
		this.secret = options.secret;
		this.timeoutMs = options.timeoutMs ?? 10_000;
		this.reconnect = options.reconnect ?? true;
		this.WebSocketImpl = options.WebSocket ?? globalThis.WebSocket;
		this.fetchImpl = options.fetch ?? globalThis.fetch.bind(globalThis);
	}

	/** Opens the WebSocket used for notifications and low-latency calls. Resolves once it is open. */
	connect(): Promise<void> {
		this.closed = false;
		if (this.socket && this.socket.readyState === this.socket.OPEN) {
			return Promise.resolve();
		}
		if (!this.WebSocketImpl) {
			return Promise.reject(
				new Error("WebSocket is not available in this environment"),
			);
		}
		return new Promise((resolve, reject) => {
			const socket = new (this.WebSocketImpl as typeof WebSocket)(this.wsUrl);
			this.socket = socket;
			let opened = false;
			socket.onopen = () => {
				opened = true;
				this.retryDelay = 500;
				this.emitConnection(true);
				resolve();
			};
			socket.onmessage = (event) => this.handleMessage(String(event.data));
			socket.onerror = () => {
				if (!opened) {
					reject(new Error(`Cannot connect to ${this.wsUrl}`));
				}
			};
			socket.onclose = () => {
				if (this.socket === socket) {
					this.socket = null;
				}
				if (opened) {
					this.emitConnection(false);
				}
				this.scheduleReconnect();
			};
		});
	}

	/** Closes the WebSocket, stops reconnecting and rejects calls still waiting on it. */
	close(): void {
		this.closed = true;
		if (this.retryTimer) {
			clearTimeout(this.retryTimer);
			this.retryTimer = null;
		}
		this.socket?.close();
		this.socket = null;
		for (const [id, pending] of this.pending) {
			clearTimeout(pending.timer);
			pending.reject(new Error("Client closed"));
			this.pending.delete(id);
		}
	}

	/** True while the WebSocket is open. */
	get connected(): boolean {
		return this.socket !== null && this.socket.readyState === this.socket.OPEN;
	}

	/** Subscribes to download notifications; returns an unsubscribe function. */
	onEvent(handler: Aria2EventHandler): () => void {
		this.handlers.add(handler);
		return () => this.handlers.delete(handler);
	}

	/** Subscribes to WebSocket open/close transitions; returns an unsubscribe function. */
	onConnectionChange(listener: (open: boolean) => void): () => void {
		this.connectionListeners.add(listener);
		return () => this.connectionListeners.delete(listener);
	}

	/** Sends a raw JSON-RPC call, prefixing the secret token when one is configured. */
	async call<T>(method: string, params: unknown[] = []): Promise<T> {
		const id = `raiju-${++this.nextId}`;
		const fullParams =
			this.secret && !method.startsWith("system.list")
				? [`token:${this.secret}`, ...params]
				: params;
		const body = JSON.stringify({
			jsonrpc: "2.0",
			id,
			method,
			params: fullParams,
		});
		if (this.connected && this.socket) {
			const socket = this.socket;
			return new Promise<T>((resolve, reject) => {
				const timer = setTimeout(() => {
					this.pending.delete(id);
					reject(new Error(`${method} timed out after ${this.timeoutMs} ms`));
				}, this.timeoutMs);
				this.pending.set(id, {
					resolve: resolve as (value: unknown) => void,
					reject,
					timer,
				});
				socket.send(body);
			});
		}
		const response = await this.fetchImpl(this.httpUrl, {
			method: "POST",
			headers: { "Content-Type": "application/json" },
			body,
			signal: AbortSignal.timeout(this.timeoutMs),
		});
		const message = (await response.json()) as RpcMessage;
		if (message.error) {
			throw new Aria2RpcError(message.error.code, message.error.message);
		}
		return message.result as T;
	}

	/** Adds HTTP(S)/FTP/magnet URIs pointing at the same resource; returns the new GID. */
	addUri(
		uris: string[],
		options: Aria2Options = {},
		position?: number,
	): Promise<Aria2Gid> {
		return this.call(
			"aria2.addUri",
			position === undefined ? [uris, options] : [uris, options, position],
		);
	}

	/** Adds a torrent from base64-encoded `.torrent` contents; returns the new GID. */
	addTorrent(
		torrentBase64: string,
		webSeeds: string[] = [],
		options: Aria2Options = {},
	): Promise<Aria2Gid> {
		return this.call("aria2.addTorrent", [torrentBase64, webSeeds, options]);
	}

	/** Removes a download, stopping it first if active. */
	remove(gid: Aria2Gid, force = false): Promise<Aria2Gid> {
		return this.call(force ? "aria2.forceRemove" : "aria2.remove", [gid]);
	}

	/** Pauses one download. */
	pause(gid: Aria2Gid, force = false): Promise<Aria2Gid> {
		return this.call(force ? "aria2.forcePause" : "aria2.pause", [gid]);
	}

	/** Pauses every active and waiting download. */
	pauseAll(force = false): Promise<"OK"> {
		return this.call(force ? "aria2.forcePauseAll" : "aria2.pauseAll");
	}

	/** Resumes one paused download. */
	unpause(gid: Aria2Gid): Promise<Aria2Gid> {
		return this.call("aria2.unpause", [gid]);
	}

	/** Resumes every paused download. */
	unpauseAll(): Promise<"OK"> {
		return this.call("aria2.unpauseAll");
	}

	/** Status of one download, optionally limited to some keys. */
	tellStatus(gid: Aria2Gid, keys?: Aria2StatusKey[]): Promise<Aria2Status> {
		return this.call("aria2.tellStatus", keys ? [gid, keys] : [gid]);
	}

	/** All active downloads. */
	tellActive(keys?: Aria2StatusKey[]): Promise<Aria2Status[]> {
		return this.call("aria2.tellActive", keys ? [keys] : []);
	}

	/** Waiting and paused downloads, `num` items from `offset`. */
	tellWaiting(
		offset: number,
		num: number,
		keys?: Aria2StatusKey[],
	): Promise<Aria2Status[]> {
		return this.call(
			"aria2.tellWaiting",
			keys ? [offset, num, keys] : [offset, num],
		);
	}

	/** Completed, errored and removed downloads, `num` items from `offset`. */
	tellStopped(
		offset: number,
		num: number,
		keys?: Aria2StatusKey[],
	): Promise<Aria2Status[]> {
		return this.call(
			"aria2.tellStopped",
			keys ? [offset, num, keys] : [offset, num],
		);
	}

	/** Global speeds and counts. */
	getGlobalStat(): Promise<Aria2GlobalStat> {
		return this.call("aria2.getGlobalStat");
	}

	/** Changes options of one download while it exists. */
	changeOption(gid: Aria2Gid, options: Aria2Options): Promise<"OK"> {
		return this.call("aria2.changeOption", [gid, options]);
	}

	/** Changes global options such as speed limits and concurrency. */
	changeGlobalOption(options: Aria2Options): Promise<"OK"> {
		return this.call("aria2.changeGlobalOption", [options]);
	}

	/** Engine version and compiled-in features. */
	getVersion(): Promise<Aria2Version> {
		return this.call("aria2.getVersion");
	}

	private handleMessage(raw: string): void {
		let message: RpcMessage;
		try {
			message = JSON.parse(raw) as RpcMessage;
		} catch {
			return;
		}
		if (message.id !== undefined) {
			const pending = this.pending.get(message.id);
			if (!pending) {
				return;
			}
			clearTimeout(pending.timer);
			this.pending.delete(message.id);
			if (message.error) {
				pending.reject(
					new Aria2RpcError(message.error.code, message.error.message),
				);
			} else {
				pending.resolve(message.result);
			}
			return;
		}
		if (message.method && (events as string[]).includes(message.method)) {
			const event = message.method as Aria2Event;
			for (const gid of eventGids(message.params)) {
				for (const handler of this.handlers) {
					handler(gid, event);
				}
			}
		}
	}

	private emitConnection(open: boolean): void {
		for (const listener of this.connectionListeners) {
			listener(open);
		}
	}

	private scheduleReconnect(): void {
		if (this.closed || !this.reconnect || this.retryTimer) {
			return;
		}
		this.retryTimer = setTimeout(() => {
			this.retryTimer = null;
			this.connect().catch(() => this.scheduleReconnect());
		}, this.retryDelay);
		this.retryDelay = Math.min(this.retryDelay * 2, 10_000);
	}
}
