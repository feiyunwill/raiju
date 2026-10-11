import { describe, expect, test } from "bun:test";
import {
	apiJobToJob,
	classifyKind,
	jobErrorText,
	kindBadge,
	pairingErrorText,
	probeSummary,
	uiKindOf,
} from "./map";
import type { ApiJob, ApiProbe } from "./types";

function jobFrom(overrides: Partial<ApiJob> = {}): ApiJob {
	return {
		id: "abc123",
		stage: "downloading",
		uiStage: "download",
		status: "active",
		titleId: "PPSA25380",
		sourceName: "PPSA25380-elden-ring-nightreign.7z",
		name: "PPSA25380-elden-ring-nightreign.7z",
		kind: "7z",
		installName: "PPSA25380.exfat",
		bytesTotal: 68_400_000_000,
		bytesStageTotal: 68_400_000_000,
		bytesDone: 29_200_000_000,
		error: "",
		...overrides,
	};
}

describe("classifyKind", () => {
	test("groups archive kinds with the format to show", () => {
		expect(classifyKind("7z")).toEqual({ group: "archive", format: "7z" });
		expect(classifyKind("zstd")).toEqual({ group: "archive", format: "zstd" });
	});

	test("maps image magic kinds to the extension ShadowMountPlus mounts", () => {
		expect(classifyKind("exfat")).toEqual({ group: "image", format: "exfat" });
		expect(classifyKind("ufs2")).toEqual({ group: "image", format: "ffpkg" });
		expect(classifyKind("pfsc")).toEqual({ group: "image", format: "ffpfsc" });
	});

	test("falls back to FILE for a kind it doesn't know", () => {
		expect(classifyKind("unknown")).toEqual({ group: "other", format: "file" });
		expect(classifyKind("")).toEqual({ group: "other", format: "file" });
	});
});

describe("kindBadge", () => {
	test("uses the API kind and prefix", () => {
		expect(kindBadge("7z")).toBe("ARCHIVE · 7z");
		expect(kindBadge("ufs2")).toBe("IMAGE · ffpkg");
	});

	test("prefers the extension the file actually carries", () => {
		expect(kindBadge("ufs2", "PPSA10872.ffpkg")).toBe("IMAGE · ffpkg");
		expect(kindBadge("exfat", "CUSA00003.EXFAT")).toBe("IMAGE · exfat");
	});
});

describe("uiKindOf", () => {
	test("keeps an archive an archive when it installs as an image", () => {
		expect(uiKindOf("7z", "PPSA25380.exfat", "game.7z")).toEqual({
			kind: "archive",
			format: "7z",
		});
	});

	test("reads a folder dump from an install name with no extension", () => {
		expect(uiKindOf("zip", "CUSA31544", "starfall.zip")).toEqual({
			kind: "folder",
			format: "zip",
		});
	});

	test("takes the image extension from the install name", () => {
		expect(uiKindOf("ufs2", "PPSA10872.ffpkg", "hollow.ffpkg")).toEqual({
			kind: "image",
			format: "ffpkg",
		});
		expect(uiKindOf("exfat", "CUSA00003.exfat", "driveclub")).toEqual({
			kind: "image",
			format: "exfat",
		});
	});

	test("guesses from the file name when the API knows nothing", () => {
		expect(uiKindOf("unknown", "", "PPSA24003.ffpkg")).toEqual({
			kind: "image",
			format: "ffpkg",
		});
	});
});

describe("apiJobToJob", () => {
	test("carries the stage and the download's byte figures", () => {
		const job = apiJobToJob(jobFrom());
		expect(job.stage).toBe("download");
		expect(job.status).toBe("active");
		expect(job.totalBytes).toBe(68_400_000_000);
		expect(job.stageDoneBytes).toBe(29_200_000_000);
		expect(job.installName).toBe("PPSA25380.exfat");
		expect(job.kind).toBe("archive");
		expect(job.format).toBe("7z");
	});

	test("merges the engine's speed, connections and paused state", () => {
		const job = apiJobToJob(jobFrom(), {
			speed: 84_000_000,
			connections: 16,
			status: "paused",
			uris: ["https://example.com/game.7z"],
		});
		expect(job.speed).toBe(84_000_000);
		expect(job.connections).toBe(16);
		expect(job.status).toBe("paused");
		expect(job.uris).toEqual(["https://example.com/game.7z"]);
	});

	test("turns a status code into a sentence", () => {
		const failed = apiJobToJob(
			jobFrom({
				status: "failed",
				stage: "failed",
				uiStage: "verify",
				error: "invalid",
			}),
		);
		expect(failed.error).toBe(
			"Raiju couldn't read this download. Check the link and retry.",
		);
		expect(
			apiJobToJob(jobFrom({ status: "active", error: "" })).error,
		).toBeUndefined();
	});

	test("falls back to the title ID in the file name", () => {
		expect(apiJobToJob(jobFrom({ titleId: "" })).titleId).toBe("PPSA25380");
		expect(apiJobToJob(jobFrom({ titleId: "UNKNOWN" })).titleId).toBe(
			"PPSA25380",
		);
	});
});

describe("probeSummary", () => {
	const verdict: ApiProbe = {
		size: 38_000_000_000,
		kind: "7z",
		category: "archive",
		uncompressedSize: 41_000_000_000,
		uncompressedKnown: true,
		estimated: false,
		fits: true,
		supported: true,
		requiredBytes: 80_000_000_000,
		freeBytes: 612_000_000_000,
	};

	test("reads as the Add-download row", () => {
		const summary = probeSummary(verdict);
		expect(summary.badge).toBe("ARCHIVE · 7z");
		expect(summary.size).toBe("38.0 GB");
		expect(summary.extracted).toBe("→ 41.0 GB");
		expect(summary.required).toBe("needs 80.0 GB");
		expect(summary.verdict).toBe("fits (612 GB free)");
		expect(summary.category).toBe("archive");
		expect(summary.destructive).toBe(false);
	});

	test("says how much more a job that doesn't fit needs", () => {
		const summary = probeSummary({
			...verdict,
			fits: false,
			requiredBytes: 100_000_000_000,
			freeBytes: 20_000_000_000,
		});
		expect(summary.verdict).toBe("needs 80.0 GB more");
		expect(summary.destructive).toBe(true);
	});

	test("says when the console cannot mount the format at all", () => {
		const summary = probeSummary({
			...verdict,
			supported: false,
			unsupportedReason: "ShadowMountPlus does not mount .ffpkg images yet",
		});
		expect(summary.verdict).toBe(
			"ShadowMountPlus does not mount .ffpkg images yet",
		);
		expect(summary.destructive).toBe(true);
	});

	test("marks a size it could only estimate, and one it doesn't know", () => {
		expect(probeSummary({ ...verdict, estimated: true }).required).toBe(
			"needs 80.0 GB (estimated)",
		);
		expect(
			probeSummary({
				...verdict,
				uncompressedKnown: false,
				uncompressedSize: 0,
			}).extracted,
		).toBe("→ size unknown");
	});
});

describe("error sentences", () => {
	test("maps processor codes the UI can hit", () => {
		expect(jobErrorText("no_space")).toStartWith("Not enough space");
		expect(jobErrorText("store_error")).toStartWith("Raiju couldn't save");
	});

	test("never shows a bare ok, and passes through anything unknown", () => {
		expect(jobErrorText("ok")).toBe("The job failed.");
		expect(jobErrorText("io error")).toBe("io error");
	});

	test("maps the pairing codes", () => {
		expect(pairingErrorText("wrong code")).toStartWith(
			"That code didn't match",
		);
		expect(pairingErrorText("too many attempts")).toStartWith(
			"Too many attempts",
		);
		expect(pairingErrorText("something new")).toStartWith(
			"Raiju couldn't pair",
		);
	});

	test("explains a probe that was refused while another was running", () => {
		expect(jobErrorText("probe_busy")).toStartWith("Raiju is already checking");
	});
});
