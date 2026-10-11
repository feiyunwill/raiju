import type { Aria2Status } from "@/aria2";
import { baseName } from "./describe";
import type { JobFile } from "./types";

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
