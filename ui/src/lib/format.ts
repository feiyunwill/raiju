const units = ["B", "KB", "MB", "GB", "TB"];

/** Formats a byte count with one decimal below 100 and none above, e.g. `38.4 GB`. */
export function formatBytes(bytes: number): string {
	let value = bytes;
	let unit = 0;
	while (value >= 1000 && unit < units.length - 1) {
		value /= 1000;
		unit++;
	}
	return `${value < 100 && unit > 0 ? value.toFixed(1) : Math.round(value)} ${units[unit]}`;
}

/** Splits a speed into its number and unit so the number can be styled on its own. */
export function splitSpeed(bytesPerSecond: number): {
	value: string;
	unit: string;
} {
	const [value, unit] = formatBytes(bytesPerSecond).split(" ");
	return { value, unit: `${unit}/s` };
}

/** Formats a speed in bytes per second, e.g. `84.2 MB/s`. */
export function formatSpeed(bytesPerSecond: number): string {
	const { value, unit } = splitSpeed(bytesPerSecond);
	return `${value} ${unit}`;
}

/** Formats a remaining duration compactly, e.g. `12m 04s` or `1h 03m`. */
export function formatDuration(seconds: number): string {
	if (!Number.isFinite(seconds) || seconds <= 0) {
		return "—";
	}
	const total = Math.round(seconds);
	const hours = Math.floor(total / 3600);
	const minutes = Math.floor((total % 3600) / 60);
	const secs = total % 60;
	if (hours > 0) {
		return `${hours}h ${String(minutes).padStart(2, "0")}m`;
	}
	return `${minutes}m ${String(secs).padStart(2, "0")}s`;
}

/** Formats a percentage with one decimal, e.g. `42.7%`. */
export function formatPercent(done: number, total: number): string {
	if (total <= 0) {
		return "—";
	}
	return `${((done / total) * 100).toFixed(1)}%`;
}
