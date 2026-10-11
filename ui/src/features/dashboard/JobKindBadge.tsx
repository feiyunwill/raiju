import type { Job } from "@/data/types";
import { jobKindLabel } from "./jobText";

/** Outlined monochrome kind pill, e.g. `ARCHIVE · 7z`. */
export function JobKindBadge({
	job,
	className,
}: {
	job: Job;
	className?: string;
}) {
	return (
		<span
			className={`text-muted-foreground inline-flex h-5 shrink-0 items-center rounded-full px-2 font-mono text-[10px] tracking-wider whitespace-nowrap ring-1 ring-foreground/15 ${className ?? ""}`}
		>
			{jobKindLabel(job)}
		</span>
	);
}
