import type { FileAllocation } from "./types";

/**
 * The allocation modes the console's filesystem can implement: `none` writes
 * straight into a fresh file, `trunc` pre-truncates it into a sparse one.
 * `posix_fallocate` is not implemented on `/data`, so aria2's `falloc` and
 * `prealloc` can never be offered.
 */
export const fileAllocationModes: { id: FileAllocation; label: string }[] = [
	{ id: "none", label: "none" },
	{ id: "trunc", label: "trunc" },
];

/**
 * Reads aria2's `file-allocation` back into a mode the console can implement,
 * so an engine left on a mode it refuses cannot reach the settings screen.
 */
export function fileAllocationFromOption(
	value: string | undefined,
): FileAllocation {
	return value === "trunc" ? "trunc" : "none";
}
