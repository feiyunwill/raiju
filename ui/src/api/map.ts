import { describeSource } from "@/data/describe";
import type {
	Job,
	JobKind,
	JobStatus,
	ProbeVerdict,
	Stage,
} from "@/data/types";
import { formatBytes } from "@/lib/format";
import type { ApiJob, ApiProbe } from "./types";

const imageFormats: Record<string, string> = {
	exfat: "exfat",
	ufs2: "ffpkg",
	ffpkg: "ffpkg",
	ffpfs: "ffpfs",
	ffpfsc: "ffpfsc",
	pfs: "ffpfs",
	pfsc: "ffpfsc",
};

const archiveFormats = ["7z", "zip", "rar", "zstd", "xz", "gzip"];

const knownImageExtensions = ["exfat", "ffpkg", "ffpfs", "ffpfsc"];

const jobErrors: Record<string, string> = {
	no_space: "Not enough space for this download. Free some space and retry.",
	full: "The console's storage is full. Free some space and retry.",
	duplicate: "This download is already in the queue.",
	wrong_stage: "That action doesn't apply at this stage.",
	not_found: "Raiju lost track of this download. Remove it and add it again.",
	invalid: "Raiju couldn't read this download. Check the link and retry.",
	store_error: "Raiju couldn't save its job list. Check free space and retry.",
	probe_busy: "Raiju is already checking another link. Try again in a moment.",
};

const pairingErrors: Record<string, string> = {
	"missing code": "Enter the 4-digit code shown on your PS5.",
	"too many attempts": "Too many attempts. Wait a minute and try again.",
	"wrong code": "That code didn't match. Check the code on your PS5.",
	"could not issue token": "Raiju couldn't create a device token. Try again.",
	unreachable: "Can't reach your PS5. Make sure it's on and Raiju is running.",
};

function extensionOf(name: string): string {
	return (name.match(/\.([a-z0-9]+)$/i)?.[1] ?? "").toLowerCase();
}

/** Groups one API kind into the UI's image/archive/file idea and names the format to show. */
export function classifyKind(kind: string): {
	group: "image" | "archive" | "other";
	format: string;
} {
	if (kind in imageFormats) {
		return { group: "image", format: imageFormats[kind] };
	}
	if (archiveFormats.includes(kind)) {
		return { group: "archive", format: kind };
	}
	return {
		group: "other",
		format: kind === "" || kind === "unknown" ? "file" : kind,
	};
}

/** Badge text for a kind, e.g. `ARCHIVE · 7z` or `IMAGE · ffpkg`. */
export function kindBadge(kind: string, name = ""): string {
	const { group, format } = classifyKind(kind);
	const extension = extensionOf(name);
	const label =
		group === "image" && knownImageExtensions.includes(extension)
			? extension
			: format;
	const prefix =
		group === "image" ? "IMAGE" : group === "archive" ? "ARCHIVE" : "FILE";
	return `${prefix} · ${label}`;
}

/**
 * The UI's kind and format for a job. An archive that installs as `<TITLE_ID>` with no extension
 * is a folder dump, which the badge shows as `FOLDER` and the stepper treats like an archive.
 */
export function uiKindOf(
	kind: string,
	installName: string,
	sourceName: string,
): { kind: JobKind; format: string } {
	const { group, format } = classifyKind(kind);
	if (group === "image") {
		const extension = [installName, sourceName]
			.map(extensionOf)
			.find((value) => knownImageExtensions.includes(value));
		return { kind: "image", format: extension ?? format };
	}
	if (group === "archive" && installName.includes(".")) {
		return { kind: "archive", format };
	}
	if (group === "archive") {
		return { kind: "folder", format };
	}
	const description = describeSource(sourceName);
	return { kind: description.kind, format: description.format };
}

/** Sentence for a gateway error code, e.g. `no_space`; a code it doesn't know passes through. */
export function jobErrorText(code: string): string {
	return jobErrors[code] ?? (code && code !== "ok" ? code : "The job failed.");
}

/** Sentence for a `POST /api/pair` error code. */
export function pairingErrorText(code: string): string {
	return pairingErrors[code] ?? "Raiju couldn't pair with your PS5. Try again.";
}

/** Live figures the API does not carry, taken from the engine's own status for the same GID. */
export type LiveFigures = {
	speed?: number;
	connections?: number;
	status?: JobStatus;
	uris?: string[];
};

/**
 * Maps one API job to a dashboard job. `bytesTotal` is the download's expected size, `bytesStageTotal`
 * the total of the stage running now and `bytesDone` that stage's progress, so a stage whose total the
 * API does not know reads as indeterminate rather than as a made-up percentage. `displayName` is the
 * game's own name when the console read one from param.json, which beats the name inferred from the
 * file, while `sourceName` keeps the file the user downloaded. A job that waits in the install stage is
 * waiting on a name conflict; one that waits in the verify stage is waiting for a title ID.
 */
export function apiJobToJob(job: ApiJob, live: LiveFigures = {}): Job {
	const description = describeSource(job.sourceName);
	const { kind, format } = uiKindOf(job.kind, job.installName, job.sourceName);
	const titleId =
		job.titleId && job.titleId !== "UNKNOWN"
			? job.titleId
			: description.titleId;
	return {
		id: job.id,
		titleId,
		name: job.displayName || description.name,
		sourceName: job.sourceName,
		kind,
		format,
		installName: job.installName || description.installName,
		stage: job.uiStage as Stage,
		status: live.status ?? (job.status as JobStatus),
		totalBytes: job.bytesTotal,
		stageBytes: job.bytesStageTotal,
		stageDoneBytes: job.bytesDone,
		speed: live.speed ?? 0,
		connections: live.connections ?? 0,
		error: job.status === "failed" ? jobErrorText(job.error) : undefined,
		uris: live.uris,
	};
}

/** Turns a probe verdict into the parts of the Add-download row, e.g. `fits (612 GB free)`. */
export function probeSummary(probe: ApiProbe): ProbeVerdict {
	const estimated = probe.estimated ? " (estimated)" : "";
	const unsupported = !probe.supported;
	return {
		badge: kindBadge(probe.kind),
		size: formatBytes(probe.size),
		extracted: probe.uncompressedKnown
			? `→ ${formatBytes(probe.uncompressedSize)}`
			: "→ size unknown",
		required: `needs ${formatBytes(probe.requiredBytes)}${estimated}`,
		verdict: unsupported
			? (probe.unsupportedReason ?? "the console cannot mount this format")
			: probe.fits
				? `fits (${formatBytes(probe.freeBytes)} free)`
				: `needs ${formatBytes(Math.max(0, probe.requiredBytes - probe.freeBytes))} more`,
		destructive: unsupported || !probe.fits,
		category: probe.category,
	};
}
