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

/** True while the pipeline waits for a name conflict to be answered. */
export function jobHasConflict(job: Job): boolean {
	return job.status === "waiting" && job.stage === "install";
}

/** True while the pipeline waits for a title ID that neither `param.json` nor the name carried. */
export function jobNeedsTitleId(job: Job): boolean {
	return job.status === "waiting" && job.stage === "verify";
}

/** Present-continuous verb for the stage running now, e.g. `extracting`. */
export function jobStageVerb(job: Job): string {
	return stageVerbs[job.stage].toLowerCase();
}

/** Percentage of the stage running now, or null when the API carries no size for that stage. */
export function jobStageProgress(job: Job): number | null {
	return (job.stage === "download" || job.stage === "extract") &&
		job.stageBytes > 0
		? (job.stageDoneBytes / job.stageBytes) * 100
		: null;
}

/** Figure for the stage running now: a percentage, or bytes moved when its total is unknown. */
export function jobStageFigure(job: Job): string {
	return jobStageProgress(job) === null
		? formatBytes(job.stageDoneBytes)
		: formatPercent(job.stageDoneBytes, job.stageBytes);
}

/** One-line status for a job, e.g. `Extracting · 1.4 GB · 142 MB/s`. */
export function jobStatusLine(job: Job): string {
	if (job.status === "failed") {
		return `Failed while ${jobStageVerb(job)}`;
	}
	if (job.status === "done") {
		return job.stage === "ready"
			? `Installed as ${job.installName}`
			: `Downloaded · ${formatBytes(job.totalBytes)}`;
	}
	if (jobHasConflict(job)) {
		return `Waiting for a name · ${formatBytes(job.totalBytes)}`;
	}
	if (jobNeedsTitleId(job)) {
		return `Waiting for a title ID · ${formatBytes(job.totalBytes)}`;
	}
	if (job.status === "waiting") {
		return `Waiting · ${formatBytes(job.totalBytes)}`;
	}
	const parts = [
		job.status === "paused"
			? `Paused while ${jobStageVerb(job)}`
			: stageVerbs[job.stage],
		jobStageFigure(job),
	];
	if (job.status !== "paused" && job.speed > 0) {
		parts.push(formatSpeed(job.speed));
	}
	return parts.join(" · ");
}

/** Remaining time for the current stage of an active job. `speed` is the download's rate, so only a
 * download gets an estimate; extraction progress is not a byte rate the same way. */
export function jobEta(job: Job): string {
	if (job.status !== "active" || job.stage !== "download" || job.speed <= 0) {
		return "—";
	}
	const percent = jobStageProgress(job);
	if (percent === null) {
		return "—";
	}
	return formatDuration((job.stageBytes - job.stageDoneBytes) / job.speed);
}

/** Kind badge text, e.g. `ARCHIVE · 7z`. */
export function jobKindLabel(job: Job): string {
	return `${job.kind.toUpperCase()} · ${job.format}`;
}
