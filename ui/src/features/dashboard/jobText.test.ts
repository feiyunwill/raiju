import { describe, expect, test } from "bun:test";
import type { Job } from "../../data/types";
import {
	jobEta,
	jobHasConflict,
	jobKindLabel,
	jobNeedsTitleId,
	jobStageFigure,
	jobStageProgress,
	jobStageVerb,
	jobStatusLine,
} from "./jobText";

function jobFrom(overrides: Partial<Job> = {}): Job {
	return {
		id: "a1",
		titleId: "PPSA25380",
		name: "Elden Ring Nightreign",
		sourceName: "PPSA25380-elden-ring-nightreign.7z",
		kind: "archive",
		format: "7z",
		installName: "PPSA25380.exfat",
		stage: "download",
		status: "active",
		totalBytes: 100_000_000_000,
		stageBytes: 100_000_000_000,
		stageDoneBytes: 50_000_000_000,
		speed: 84_000_000,
		connections: 16,
		...overrides,
	};
}

describe("waiting states", () => {
	test("a name conflict waits in install, a missing title ID in verify", () => {
		expect(
			jobHasConflict(jobFrom({ status: "waiting", stage: "install" })),
		).toBe(true);
		expect(
			jobNeedsTitleId(jobFrom({ status: "waiting", stage: "verify" })),
		).toBe(true);
		expect(
			jobHasConflict(jobFrom({ status: "waiting", stage: "download" })),
		).toBe(false);
	});
});

describe("stage figures", () => {
	test("a download has a percentage, every other stage doesn't", () => {
		expect(jobStageProgress(jobFrom())).toBe(50);
		expect(jobStageProgress(jobFrom({ stage: "extract" }))).toBeNull();
		expect(
			jobStageProgress(jobFrom({ stage: "download", stageBytes: 0 })),
		).toBeNull();
	});

	test("figures fall back to bytes moved when the stage has no total", () => {
		expect(jobStageFigure(jobFrom())).toBe("50.0%");
		expect(
			jobStageFigure(
				jobFrom({ stage: "extract", stageDoneBytes: 1_500_000_000 }),
			),
		).toBe("1.5 GB");
	});

	test("remaining time only exists while a download is running", () => {
		expect(jobEta(jobFrom())).toBe("9m 55s");
		expect(jobEta(jobFrom({ stage: "extract" }))).toBe("—");
		expect(jobEta(jobFrom({ status: "paused" }))).toBe("—");
	});
});

describe("jobStatusLine", () => {
	test("names the stage that is running", () => {
		expect(jobStatusLine(jobFrom())).toBe("Downloading · 50.0% · 84.0 MB/s");
		expect(jobStageVerb(jobFrom({ stage: "extract" }))).toBe("extracting");
		expect(
			jobStatusLine(
				jobFrom({
					stage: "extract",
					stageDoneBytes: 1_500_000_000,
					speed: 142_000_000,
				}),
			),
		).toBe("Extracting · 1.5 GB · 142 MB/s");
	});

	test("says why a job is waiting", () => {
		expect(
			jobStatusLine(jobFrom({ status: "waiting", stage: "install" })),
		).toBe("Waiting for a name · 100 GB");
		expect(jobStatusLine(jobFrom({ status: "waiting", stage: "verify" }))).toBe(
			"Waiting for a title ID · 100 GB",
		);
		expect(jobStatusLine(jobFrom({ status: "waiting" }))).toBe(
			"Waiting · 100 GB",
		);
	});

	test("keeps the paused, failed and done wording", () => {
		expect(jobStatusLine(jobFrom({ status: "paused" }))).toBe(
			"Paused while downloading · 50.0%",
		);
		expect(jobStatusLine(jobFrom({ status: "failed", stage: "extract" }))).toBe(
			"Failed while extracting",
		);
		expect(jobStatusLine(jobFrom({ status: "done", stage: "ready" }))).toBe(
			"Installed as PPSA25380.exfat",
		);
	});
});

describe("jobKindLabel", () => {
	test("reads as the kind badge", () => {
		expect(jobKindLabel(jobFrom())).toBe("ARCHIVE · 7z");
		expect(jobKindLabel(jobFrom({ kind: "image", format: "ffpkg" }))).toBe(
			"IMAGE · ffpkg",
		);
		expect(jobKindLabel(jobFrom({ kind: "folder", format: "zip" }))).toBe(
			"FOLDER · zip",
		);
	});
});
