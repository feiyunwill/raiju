import { readdirSync, readFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { gzipSync } from "bun";

const budgetBytes = 300 * 1024;
const distDir = join(import.meta.dir, "..", "dist");

/** Recursively lists every file under a directory. */
function listFiles(dir: string): string[] {
	return readdirSync(dir).flatMap((name) => {
		const path = join(dir, name);
		return statSync(path).isDirectory() ? listFiles(path) : [path];
	});
}

const sizes = listFiles(distDir).map((path) => ({
	path: path.slice(distDir.length + 1),
	gzip: gzipSync(readFileSync(path)).length,
}));
const total = sizes.reduce((sum, file) => sum + file.gzip, 0);

for (const file of sizes.sort((a, b) => b.gzip - a.gzip)) {
	console.log(`${(file.gzip / 1024).toFixed(1).padStart(8)} KB  ${file.path}`);
}
console.log(
	`${(total / 1024).toFixed(1).padStart(8)} KB  total gzipped (budget ${budgetBytes / 1024} KB)`,
);

if (total > budgetBytes) {
	console.error("UI bundle exceeds its size budget");
	process.exit(1);
}
