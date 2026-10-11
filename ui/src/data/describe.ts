import type { JobKind } from "./types";

const imageFormats = ["ffpkg", "exfat", "ffpfs", "ffpfsc"];

export type SourceDescription = {
	sourceName: string;
	titleId: string;
	name: string;
	kind: JobKind;
	format: string;
	installName: string;
};

/** Returns the last path segment of a URL or file path, decoded. */
export function baseName(pathOrUrl: string): string {
	const clean = pathOrUrl.split(/[?#]/)[0] ?? pathOrUrl;
	const segment = clean.split(/[\\/]/).filter(Boolean).pop() ?? clean;
	try {
		return decodeURIComponent(segment);
	} catch {
		return segment;
	}
}

/** Guesses title ID, display name, kind and install name from a downloaded file name. */
export function describeSource(sourceName: string): SourceDescription {
	const format = (
		sourceName.match(/\.([a-z0-9]+)$/i)?.[1] ?? "bin"
	).toLowerCase();
	const titleId = sourceName.match(/(PPSA|CUSA)\d{5}/)?.[0] ?? "UNKNOWN";
	const kind: JobKind = imageFormats.includes(format) ? "image" : "archive";
	const name = sourceName
		.replace(/\.[^.]+$/, "")
		.replace(/(PPSA|CUSA)\d{5}/, "")
		.replace(/[-_.]+/g, " ")
		.trim()
		.replace(/\b\w/g, (c) => c.toUpperCase());
	return {
		sourceName,
		titleId,
		name: name || titleId,
		kind,
		format,
		installName: kind === "image" ? `${titleId}.${format}` : `${titleId}.exfat`,
	};
}
