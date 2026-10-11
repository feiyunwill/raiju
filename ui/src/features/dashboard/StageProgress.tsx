import { cn } from "cn";
import { Progress } from "@/components/ui/progress";
import type { Job } from "@/data/types";
import { jobStageProgress } from "./jobText";

/** Bar for the stage running now: a value when the stage has one, a glide when it hasn't. */
export function StageProgress({
	job,
	className,
}: {
	job: Job;
	className?: string;
}) {
	const percent = jobStageProgress(job);
	if (percent === null) {
		return (
			<div
				aria-hidden
				className={cn(
					"bg-muted relative h-1.5 overflow-hidden rounded-full",
					className,
				)}
			>
				<span className="bg-primary/70 animate-[raiju-indeterminate_1.4s_ease-in-out_infinite] absolute inset-y-0 w-1/3 rounded-full" />
			</div>
		);
	}
	return <Progress value={percent} className={cn("h-1.5", className)} />;
}
