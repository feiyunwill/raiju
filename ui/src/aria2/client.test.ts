import {
	afterAll,
	beforeAll,
	beforeEach,
	describe,
	expect,
	setDefaultTimeout,
	test,
} from "bun:test";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import type { Subprocess } from "bun";
import { Aria2Client } from "./client";
import { type Aria2Snapshot, syncAria2 } from "./sync";
import { type Aria2Event, type Aria2Gid, Aria2RpcError } from "./types";

const aria2CallTimeoutMs = 20_000;
const aria2TestTimeoutMs = 90_000;
const secret = "raiju-test-secret";

setDefaultTimeout(aria2TestTimeoutMs);
const smallFile = new Uint8Array(256 * 1024).map((_, i) => i % 251);
const slowFileSize = 64 * 1024 * 1024;

let aria2: Subprocess | undefined;
let server: ReturnType<typeof Bun.serve>;
let client: Aria2Client;
let downloadDir: string;
let rpcPort: number;
const events: { gid: Aria2Gid; event: Aria2Event }[] = [];

function freePort(): number {
	const probe = Bun.serve({ port: 0, fetch: () => new Response() });
	const port = probe.port ?? 0;
	probe.stop(true);
	return port;
}

function slowStream(size: number): ReadableStream<Uint8Array> {
	const chunk = new Uint8Array(16 * 1024);
	let sent = 0;
	return new ReadableStream({
		async pull(controller) {
			if (sent >= size) {
				controller.close();
				return;
			}
			await new Promise((resolve) => setTimeout(resolve, 20));
			const length = Math.min(chunk.length, size - sent);
			controller.enqueue(chunk.subarray(0, length));
			sent += length;
		},
	});
}

async function waitFor<T>(
	check: () => T | undefined | Promise<T | undefined>,
	timeoutMs = aria2CallTimeoutMs,
): Promise<T> {
	const deadline = Date.now() + timeoutMs;
	while (Date.now() < deadline) {
		const value = await check();
		if (value !== undefined) {
			return value;
		}
		await new Promise((resolve) => setTimeout(resolve, 50));
	}
	throw new Error("Timed out waiting for condition");
}

function sawEvent(gid: Aria2Gid, event: Aria2Event): true | undefined {
	return events.some((item) => item.gid === gid && item.event === event)
		? true
		: undefined;
}

function spawnAria2(): Subprocess {
	return Bun.spawn(
		[
			"aria2c",
			"--enable-rpc",
			`--rpc-listen-port=${rpcPort}`,
			`--rpc-secret=${secret}`,
			`--dir=${downloadDir}`,
			"--no-conf",
			"--quiet",
			"--split=1",
			"--max-connection-per-server=1",
			"--file-allocation=none",
			"--enable-dht=false",
			"--bt-enable-lpd=false",
			"--enable-peer-exchange=false",
		],
		{ stdout: "ignore", stderr: "inherit" },
	);
}

async function connectClient(): Promise<void> {
	client = new Aria2Client({
		url: `http://127.0.0.1:${rpcPort}/jsonrpc`,
		secret,
		reconnect: false,
		timeoutMs: aria2CallTimeoutMs,
	});
	await waitFor(async () => {
		try {
			await client.connect();
			return true;
		} catch {
			return undefined;
		}
	});
	client.onEvent((gid, event) => events.push({ gid, event }));
}

/**
 * Brings the engine back when a test that timed out left it dead or
 * unreachable, so one failure cannot take the rest of the file with it.
 */
async function ensureEngine(): Promise<void> {
	if (aria2 !== undefined && aria2.exitCode === null) {
		try {
			await client.getVersion();
			return;
		} catch {
			client.close();
		}
	}
	aria2 = spawnAria2();
	await connectClient();
}

/**
 * Drops whatever a previous test left behind: a test that timed out
 * mid-download would otherwise have the next one reading its state.
 */
async function purgeDownloads(): Promise<void> {
	try {
		for (const item of await client.tellActive(["gid"])) {
			await client.remove(item.gid, true);
		}
		for (const item of await client.tellWaiting(0, 100, ["gid"])) {
			await client.remove(item.gid, true);
		}
		for (const item of await client.tellStopped(0, 100, ["gid"])) {
			await client.removeDownloadResult(item.gid);
		}
	} catch {
		return;
	}
}

beforeAll(async () => {
	server = Bun.serve({
		port: 0,
		fetch(request) {
			const path = new URL(request.url).pathname;
			if (path === "/small.bin") {
				return new Response(smallFile, {
					headers: { "Content-Length": String(smallFile.length) },
				});
			}
			if (path === "/slow.bin") {
				return new Response(slowStream(slowFileSize), {
					headers: {
						"Content-Length": String(slowFileSize),
						"Accept-Ranges": "none",
					},
				});
			}
			return new Response("Not found", { status: 404 });
		},
	});
	downloadDir = mkdtempSync(join(tmpdir(), "raiju-aria2-"));
	rpcPort = freePort();
	aria2 = spawnAria2();
	await connectClient();
});

beforeEach(async () => {
	events.length = 0;
	await ensureEngine();
	await purgeDownloads();
});

afterAll(() => {
	client?.close();
	aria2?.kill();
	server?.stop(true);
	if (downloadDir) {
		rmSync(downloadDir, { recursive: true, force: true });
	}
});

describe("Aria2Client against a real aria2c", () => {
	test("getVersion reports the engine version", async () => {
		const version = await client.getVersion();
		expect(version.version).toMatch(/^\d+\.\d+\.\d+/);
		expect(Array.isArray(version.enabledFeatures)).toBe(true);
	});

	test("addUri downloads a file and emits start and complete notifications", async () => {
		const gid = await client.addUri([
			`http://127.0.0.1:${server.port}/small.bin`,
		]);
		expect(gid).toMatch(/^[0-9a-f]{16}$/);
		await waitFor(() => sawEvent(gid, "aria2.onDownloadComplete"));
		expect(sawEvent(gid, "aria2.onDownloadStart")).toBe(true);
		const stopped = await client.tellStopped(0, 100, [
			"gid",
			"status",
			"totalLength",
			"completedLength",
		]);
		const status = stopped.find((item) => item.gid === gid);
		expect(status?.status).toBe("complete");
		expect(status?.completedLength).toBe(String(smallFile.length));
	});

	test("pause, unpause, changeOption and remove control an active download", async () => {
		const gid = await client.addUri(
			[`http://127.0.0.1:${server.port}/slow.bin`],
			{ out: "slow.bin" },
		);
		await waitFor(async () => {
			const active = await client.tellActive(["gid"]);
			return active.some((item) => item.gid === gid) ? true : undefined;
		});

		expect(
			await client.changeOption(gid, { "max-download-limit": "512K" }),
		).toBe("OK");
		expect((await client.getOption(gid))["max-download-limit"]).toBe("524288");
		const fileServer = await waitFor(
			async () => (await client.getServers(gid))[0]?.servers[0],
		);
		expect(fileServer.uri).toContain("/slow.bin");
		const stat = await client.getGlobalStat();
		expect(Number(stat.numActive)).toBeGreaterThanOrEqual(1);

		expect(await client.pause(gid)).toBe(gid);
		await waitFor(() => sawEvent(gid, "aria2.onDownloadPause"));
		const waiting = await client.tellWaiting(0, 100, ["gid", "status"]);
		expect(waiting.find((item) => item.gid === gid)?.status).toBe("paused");

		expect(await client.unpause(gid)).toBe(gid);
		await waitFor(async () =>
			(await client.tellStatus(gid, ["status"])).status === "active"
				? true
				: undefined,
		);

		expect(await client.remove(gid, true)).toBe(gid);
		await waitFor(() => sawEvent(gid, "aria2.onDownloadStop"));
		const removed = await client.tellStatus(gid, ["status"]);
		expect(removed.status).toBe("removed");
		expect(await client.removeDownloadResult(gid)).toBe("OK");
		await expect(client.tellStatus(gid)).rejects.toBeInstanceOf(Aria2RpcError);
	});

	test("pauseAll and unpauseAll act on every download", async () => {
		const gid = await client.addUri(
			[`http://127.0.0.1:${server.port}/slow.bin`],
			{ out: "slow-all.bin" },
		);
		await waitFor(async () =>
			(await client.tellStatus(gid, ["status"])).status === "active"
				? true
				: undefined,
		);
		expect(await client.pauseAll(true)).toBe("OK");
		await waitFor(async () =>
			(await client.tellStatus(gid, ["status"])).status === "paused"
				? true
				: undefined,
		);
		expect(await client.unpauseAll()).toBe("OK");
		await waitFor(async () =>
			(await client.tellStatus(gid, ["status"])).status !== "paused"
				? true
				: undefined,
		);
		await client.remove(gid, true);
	});

	test("addTorrent queues a torrent from base64 contents", async () => {
		const torrent = bencodeTorrent("raiju-test.bin", 32 * 1024);
		const gid = await client.addTorrent(
			Buffer.from(torrent).toString("base64"),
			[],
			{ pause: "true" },
		);
		const status = await client.tellStatus(gid, [
			"gid",
			"status",
			"files",
			"infoHash",
			"bittorrent",
		]);
		expect(status.status).toBe("paused");
		expect(status.files[0]?.length).toBe(String(32 * 1024));
		expect(status.bittorrent?.info?.name).toBe("raiju-test.bin");
		expect(status.infoHash).toMatch(/^[0-9a-f]{40}$/);
		await client.remove(gid, true);
	});

	test("syncAria2 refetches all lists when a notification arrives", async () => {
		const snapshots: Aria2Snapshot[] = [];
		const visibility = {
			visibilityState: "hidden" as DocumentVisibilityState,
			addEventListener: () => undefined,
			removeEventListener: () => undefined,
		};
		const sync = syncAria2(client, (snapshot) => snapshots.push(snapshot), {
			visibility,
		});
		expect(snapshots.length).toBe(0);
		const gid = await client.addUri(
			[`http://127.0.0.1:${server.port}/small.bin`],
			{ out: "sync.bin" },
		);
		const done = await waitFor(() =>
			snapshots.find((snapshot) =>
				snapshot.stopped.some((item) => item.gid === gid),
			),
		);
		expect(done.stopped.find((item) => item.gid === gid)?.status).toBe(
			"complete",
		);
		expect(done.stat.numStoppedTotal).toBeDefined();
		sync.stop();
	});

	test("calls fall back to HTTP when the WebSocket is closed", async () => {
		const http = new Aria2Client({
			url: `http://127.0.0.1:${rpcPort}/jsonrpc`,
			secret,
			reconnect: false,
		});
		expect(http.connected).toBe(false);
		const stat = await http.getGlobalStat();
		expect(stat.numStoppedTotal).toBeDefined();
	});

	test("RPC errors surface as Aria2RpcError over WebSocket and HTTP", async () => {
		await expect(client.tellStatus("0000000000000000")).rejects.toBeInstanceOf(
			Aria2RpcError,
		);
		const http = new Aria2Client({
			url: `http://127.0.0.1:${rpcPort}/jsonrpc`,
			secret: "wrong",
			reconnect: false,
		});
		await expect(http.getGlobalStat()).rejects.toBeInstanceOf(Aria2RpcError);
	});
});

function bencode(value: unknown): Uint8Array {
	const encoder = new TextEncoder();
	const parts: Uint8Array[] = [];
	const write = (item: unknown): void => {
		if (typeof item === "number") {
			parts.push(encoder.encode(`i${item}e`));
		} else if (typeof item === "string") {
			const bytes = encoder.encode(item);
			parts.push(encoder.encode(`${bytes.length}:`), bytes);
		} else if (item instanceof Uint8Array) {
			parts.push(encoder.encode(`${item.length}:`), item);
		} else if (Array.isArray(item)) {
			parts.push(encoder.encode("l"));
			item.forEach(write);
			parts.push(encoder.encode("e"));
		} else if (item && typeof item === "object") {
			parts.push(encoder.encode("d"));
			for (const key of Object.keys(item).sort()) {
				write(key);
				write((item as Record<string, unknown>)[key]);
			}
			parts.push(encoder.encode("e"));
		}
	};
	write(value);
	return Buffer.concat(parts);
}

function bencodeTorrent(name: string, length: number): Uint8Array {
	const pieceLength = 16 * 1024;
	const pieces = new Uint8Array(Math.ceil(length / pieceLength) * 20);
	return bencode({
		announce: "http://127.0.0.1:1/announce",
		info: { length, name, "piece length": pieceLength, pieces },
	});
}
