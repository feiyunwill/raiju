import type { Job } from "@/data/types";
import {
	formatBytes,
	formatDuration,
	formatPercent,
	formatSpeed,
} from "@/lib/format";

const stageVerbs = {
	download: "Downloading",
	verify: "Verifying",
	extract: "Extracting",
	install: "Installing",
	ready: "Ready",
} as const;

/** One-line status for a job, e.g. `Extracting · 38.2% · 142 MB/s`. */
export function jobStatusLine(job: Job): string {
	if (job.status === "failed") {
		return `Failed while ${stageVerbs[job.stage].toLowerCase()}`;
	}
	if (job.status === "done") {
		return job.stage === "ready"
			? `Installed as ${job.installName}`
			: `Downloaded · ${formatBytes(job.totalBytes)}`;
	}
	if (job.status === "waiting") {
		return `Waiting · ${formatBytes(job.totalBytes)}`;
	}
	const percent = formatPercent(job.stageDoneBytes, job.stageBytes);
	if (job.status === "paused") {
		return `Paused · ${percent}`;
	}
	const parts = [stageVerbs[job.stage], percent];
	if (job.speed > 0) {
		parts.push(formatSpeed(job.speed));
	}
	return parts.join(" · ");
}

/** Remaining time for the current stage of an active job. */
export function jobEta(job: Job): string {
	if (job.status !== "active" || job.speed <= 0) {
		return "—";
	}
	return formatDuration((job.stageBytes - job.stageDoneBytes) / job.speed);
}

/** Kind badge text, e.g. `ARCHIVE · 7z`. */
export function jobKindLabel(job: Job): string {
	return `${job.kind.toUpperCase()} · ${job.format}`;
}
