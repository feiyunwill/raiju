import type {
	ApiDevices,
	ApiFolders,
	ApiJob,
	ApiNewJob,
	ApiPairing,
	ApiProbe,
	ApiRemovedJob,
	ApiStatus,
	ApiStorage,
	ConflictChoice,
} from "./types";

/** A failed Raiju API call: `status` is the HTTP status, or 0 when the console could not be reached. */
export class RaijuApiError extends Error {
	readonly status: number;
	readonly code: string;

	constructor(status: number, code: string, message?: string) {
		super(message ?? (code || `Raiju API error ${status}`));
		this.name = "RaijuApiError";
		this.status = status;
		this.code = code;
	}
}

export type RaijuClientOptions = {
	baseUrl?: string;
	token?: string;
	timeoutMs?: number;
	fetch?: typeof fetch;
};

export type RaijuClient = ReturnType<typeof createRaijuClient>;

/** True for the 401 the gateway returns when no valid device token is presented. */
export function isUnauthorized(error: unknown): boolean {
	return error instanceof RaijuApiError && error.status === 401;
}

/** True when the console or the gateway could not be reached at all. */
export function isUnreachable(error: unknown): boolean {
	return error instanceof RaijuApiError && error.status === 0;
}

/** True for the 409 the gateway returns when a job does not fit on the console. */
export function isNoSpace(error: unknown): boolean {
	return error instanceof RaijuApiError && error.status === 409;
}

/** Reads the gateway's `{"error":"…"}` body without letting a parse failure mask the status. */
async function errorCode(response: Response): Promise<string> {
	try {
		const body = (await response.json()) as { error?: unknown };
		return typeof body.error === "string" ? body.error : "";
	} catch {
		return "";
	}
}

/**
 * Typed client for the Raiju `/api`, covering the endpoints of ARCHITECTURE.md and nothing else.
 * Every call sends the `raiju_device` cookie; an explicit `token` is sent as a Bearer header instead,
 * for clients that are not browsers (the extension and the companion).
 */
export function createRaijuClient(options: RaijuClientOptions = {}) {
	const baseUrl = (options.baseUrl ?? "").replace(/\/$/, "");
	const fetchImpl = options.fetch ?? globalThis.fetch.bind(globalThis);
	const timeoutMs = options.timeoutMs ?? 10_000;

	async function request(
		path: string,
		init: RequestInit & { json?: unknown } = {},
	): Promise<Response> {
		const { json: body, ...rest } = init;
		const headers = new Headers(rest.headers);
		if (options.token) {
			headers.set("Authorization", `Bearer ${options.token}`);
		}
		if (body !== undefined) {
			headers.set("Content-Type", "application/json");
		}
		let response: Response;
		try {
			response = await fetchImpl(`${baseUrl}${path}`, {
				credentials: "include",
				signal: AbortSignal.timeout(timeoutMs),
				...rest,
				headers,
				body: body === undefined ? rest.body : JSON.stringify(body),
			});
		} catch (cause) {
			throw new RaijuApiError(
				0,
				"unreachable",
				cause instanceof Error ? cause.message : String(cause),
			);
		}
		if (!response.ok) {
			throw new RaijuApiError(response.status, await errorCode(response));
		}
		return response;
	}

	async function json<T>(
		path: string,
		init?: RequestInit & { json?: unknown },
	): Promise<T> {
		return (await (await request(path, init)).json()) as T;
	}

	return {
		baseUrl,
		/** Console version, uptime, firmware, address and engine health. No token needed. */
		status: () => json<ApiStatus>("/api/status"),
		storage: () => json<ApiStorage>("/api/storage"),
		/** The preset roots plus the entries of `path`, which defaults to the install root. */
		folders: (path?: string) =>
			json<ApiFolders>(
				path ? `/api/folders?path=${encodeURIComponent(path)}` : "/api/folders",
			),
		jobs: () => json<{ jobs: ApiJob[] }>("/api/jobs").then((body) => body.jobs),
		job: (id: string) => json<ApiJob>(`/api/jobs/${encodeURIComponent(id)}`),
		addJob: (input: ApiNewJob) =>
			json<ApiJob>("/api/jobs", { method: "POST", json: input }),
		retryJob: (id: string) =>
			json<ApiJob>(`/api/jobs/${encodeURIComponent(id)}/retry`, {
				method: "POST",
			}),
		/** The job leaves the queue: staging, trash and, unless it installed, its download go with it. */
		removeJob: (id: string) =>
			json<ApiRemovedJob>(`/api/jobs/${encodeURIComponent(id)}`, {
				method: "DELETE",
			}),
		cancelJob: (id: string) =>
			json<ApiJob>(`/api/jobs/${encodeURIComponent(id)}/cancel`, {
				method: "POST",
			}),
		resolveJob: (id: string, choice: ConflictChoice) =>
			json<ApiJob>(`/api/jobs/${encodeURIComponent(id)}/resolve`, {
				method: "POST",
				json: { choice },
			}),
		/** Gives a job that is waiting for a title ID one, so it can install its payload. */
		titleJob: (id: string, titleId: string) =>
			json<ApiJob>(`/api/jobs/${encodeURIComponent(id)}/title`, {
				method: "POST",
				json: { titleId },
			}),
		/** One probe at a time: the gateway answers 503 `probe_busy` while another is running. */
		probe: (uri: string) =>
			json<ApiProbe>("/api/probe", { method: "POST", json: { uri } }),
		devices: () =>
			json<ApiDevices>("/api/devices").then((body) => body.devices),
		revokeDevice: (id: string) =>
			json<{ revoked: boolean }>(`/api/devices/${encodeURIComponent(id)}`, {
				method: "DELETE",
			}),
		pair: (code: string, label: string) =>
			json<ApiPairing>("/api/pair", { method: "POST", json: { code, label } }),
		/** Toggles a payload power option. Both answer 404 until the payload ships them. */
		power: (option: "keep-awake" | "notifications", on: boolean) =>
			json<Record<string, boolean>>(`/api/power/${option}`, {
				method: "POST",
				json: { on },
			}),
	};
}
