import type { Aria2Status } from "@/aria2";
import { baseName, describeSource } from "./describe";
import type { Job, JobFile, JobStatus } from "./types";

const statusMap: Record<Aria2Status["status"], JobStatus | null> = {
	active: "active",
	waiting: "waiting",
	paused: "paused",
	complete: "done",
	error: "failed",
	removed: null,
};

/** Best human-facing name of an aria2 download: torrent name, first file, or first URI. */
export function aria2SourceName(status: Aria2Status): string {
	const torrentName = status.bittorrent?.info?.name;
	if (torrentName) {
		return torrentName;
	}
	const file = status.files?.[0];
	if (file?.path) {
		return baseName(file.path);
	}
	const uri = file?.uris?.[0]?.uri;
	return uri ? baseName(uri) : status.gid;
}

/** Maps aria2 files to the UI's file model. */
export function aria2Files(status: Aria2Status): JobFile[] {
	return (status.files ?? []).map((file) => ({
		index: Number(file.index),
		path: file.path,
		length: Number(file.length),
		completed: Number(file.completedLength),
		selected: file.selected === "true",
	}));
}

/** Converts one aria2 download to a dashboard job, or null for entries the UI hides. */
export function aria2ToJob(status: Aria2Status): Job | null {
	const mapped = statusMap[status.status];
	if (!mapped || (status.followedBy && status.followedBy.length > 0)) {
		return null;
	}
	const description = describeSource(aria2SourceName(status));
	const total = Number(status.totalLength);
	const done = Number(status.completedLength);
	const uris = [
		...new Set(
			(status.files ?? []).flatMap((file) => file.uris.map((uri) => uri.uri)),
		),
	];
	return {
		id: status.gid,
		titleId: description.titleId,
		name: description.name,
		sourceName: description.sourceName,
		kind: description.kind,
		format: description.format,
		installName: description.installName,
		stage: "download",
		status: mapped,
		totalBytes: total,
		stageBytes: total,
		stageDoneBytes: done,
		speed: Number(status.downloadSpeed),
		connections: Number(status.connections),
		error:
			mapped === "failed"
				? (status.errorMessage ??
					`aria2 error ${status.errorCode ?? ""}`.trim())
				: undefined,
		uris,
	};
}
