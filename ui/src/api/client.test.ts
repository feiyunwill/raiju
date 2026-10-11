import { describe, expect, test } from "bun:test";
import {
	createRaijuClient,
	isNoSpace,
	isUnauthorized,
	isUnreachable,
	RaijuApiError,
} from "./client";

type Call = { url: string; init: RequestInit | undefined };

function fetchStub(handler: (call: Call) => Response | Promise<Response>): {
	fetch: typeof fetch;
	calls: Call[];
} {
	const calls: Call[] = [];
	const impl = async (input: RequestInfo | URL, init?: RequestInit) => {
		const call = { url: String(input), init };
		calls.push(call);
		return handler(call);
	};
	return { fetch: impl as typeof fetch, calls };
}

function json(body: unknown, status = 200): Response {
	return new Response(JSON.stringify(body), {
		status,
		headers: { "Content-Type": "application/json" },
	});
}

function bodyOf(call: Call): unknown {
	return JSON.parse(String(call.init?.body));
}

describe("createRaijuClient", () => {
	test("asks the gateway on the same origin and sends the device cookie", async () => {
		const stub = fetchStub(() => json({ name: "raiju", version: "0.1.0" }));
		const client = createRaijuClient({ fetch: stub.fetch });
		await client.status();
		expect(stub.calls[0].url).toBe("/api/status");
		expect(stub.calls[0].init?.credentials).toBe("include");
	});

	test("takes a base URL and unwraps the jobs list", async () => {
		const stub = fetchStub(() => json({ jobs: [{ id: "a1" }] }));
		const client = createRaijuClient({
			baseUrl: "http://192.168.1.50:8080/",
			fetch: stub.fetch,
		});
		const jobs = await client.jobs();
		expect(stub.calls[0].url).toBe("http://192.168.1.50:8080/api/jobs");
		expect(jobs.map((job) => job.id)).toEqual(["a1"]);
	});

	test("sends the pairing code as a POST body", async () => {
		const stub = fetchStub(() =>
			json({ id: "d1", token: "t", label: "browser" }),
		);
		const client = createRaijuClient({ fetch: stub.fetch });
		const paired = await client.pair("4821", "browser");
		expect(stub.calls[0].url).toBe("/api/pair");
		expect(stub.calls[0].init?.method).toBe("POST");
		expect(bodyOf(stub.calls[0])).toEqual({ code: "4821", label: "browser" });
		expect(paired.id).toBe("d1");
	});

	test("sends a conflict choice to the resolve endpoint", async () => {
		const stub = fetchStub(() => json({ id: "a1" }));
		const client = createRaijuClient({ fetch: stub.fetch });
		await client.resolveJob("a1", "rename");
		expect(stub.calls[0].url).toBe("/api/jobs/a1/resolve");
		expect(bodyOf(stub.calls[0])).toEqual({ choice: "rename" });
	});

	test("toggles a power option on its own endpoint", async () => {
		const stub = fetchStub(() => json({ keepAwake: true }));
		const client = createRaijuClient({ fetch: stub.fetch });
		await client.power("keep-awake", true);
		expect(stub.calls[0].url).toBe("/api/power/keep-awake");
		expect(bodyOf(stub.calls[0])).toEqual({ on: true });
	});

	test("probes one URL and spans the preset folders", async () => {
		const stub = fetchStub(() =>
			json({ roots: [], path: "/data", entries: [] }),
		);
		const client = createRaijuClient({ fetch: stub.fetch });
		await client.probe("https://example.com/PPSA24003.7z");
		await client.folders("/data/homebrew");
		expect(stub.calls[0].url).toBe("/api/probe");
		expect(bodyOf(stub.calls[0])).toEqual({
			uri: "https://example.com/PPSA24003.7z",
		});
		expect(stub.calls[1].url).toBe("/api/folders?path=%2Fdata%2Fhomebrew");
	});

	test("sends an explicit token as a Bearer header for non-browser clients", async () => {
		const stub = fetchStub(() => json({ devices: [] }));
		const client = createRaijuClient({ token: "abc", fetch: stub.fetch });
		await client.devices();
		const headers = new Headers(stub.calls[0].init?.headers);
		expect(headers.get("Authorization")).toBe("Bearer abc");
	});

	test("a 401 reads as unpaired, not as an error", async () => {
		const stub = fetchStub(() => json({ error: "unauthorized" }, 401));
		const client = createRaijuClient({ fetch: stub.fetch });
		const failure = client.jobs().catch((error: unknown) => error);
		const error = await failure;
		expect(error).toBeInstanceOf(RaijuApiError);
		expect(isUnauthorized(error)).toBe(true);
		expect(isUnreachable(error)).toBe(false);
	});

	test("a refused connection reads as unreachable", async () => {
		const stub = fetchStub(() => {
			throw new Error("ECONNREFUSED");
		});
		const client = createRaijuClient({ fetch: stub.fetch });
		const error = await client.status().catch((failure: unknown) => failure);
		expect(isUnreachable(error)).toBe(true);
	});

	test("a 409 from POST /api/jobs reads as no space", async () => {
		const stub = fetchStub(() =>
			json(
				{ error: "no_space", requiredBytes: 1, freeBytes: 0, estimated: false },
				409,
			),
		);
		const client = createRaijuClient({ fetch: stub.fetch });
		const error = await client
			.addJob({ id: "a1", uri: "https://example.com/game.7z" })
			.catch((failure: unknown) => failure);
		expect(isNoSpace(error)).toBe(true);
		expect((error as RaijuApiError).code).toBe("no_space");
	});

	test("keeps the gateway's error string when a body isn't JSON", async () => {
		const stub = fetchStub(() => new Response("nope", { status: 404 }));
		const client = createRaijuClient({ fetch: stub.fetch });
		const error = await client.job("a1").catch((failure: unknown) => failure);
		expect((error as RaijuApiError).status).toBe(404);
		expect((error as RaijuApiError).code).toBe("");
	});
});
