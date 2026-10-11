import type { ConflictChoice } from "@/api/types";
import { Button } from "@/components/ui/button";
import type { Job } from "@/data/types";

type ConflictPromptProps = {
	job: Job;
	onResolve: (id: string, choice: ConflictChoice) => void;
};

/** Inline prompt for a job whose install name is already taken. */
export function ConflictPrompt({ job, onResolve }: ConflictPromptProps) {
	return (
		<div className="bg-foreground/[0.06] flex flex-wrap items-center gap-x-3 gap-y-2 rounded-2xl px-3 py-2 text-[13px]">
			<span className="min-w-0 flex-1 truncate">
				<span className="font-mono">{job.installName}</span> already exists
			</span>
			<div className="flex gap-1">
				<Button
					size="sm"
					variant="secondary"
					onClick={(event) => {
						event.stopPropagation();
						onResolve(job.id, "rename");
					}}
				>
					Rename
				</Button>
				<Button
					size="sm"
					variant="secondary"
					onClick={(event) => {
						event.stopPropagation();
						onResolve(job.id, "replace");
					}}
				>
					Replace
				</Button>
				<Button
					size="sm"
					variant="ghost"
					onClick={(event) => {
						event.stopPropagation();
						onResolve(job.id, "cancel");
					}}
				>
					Cancel
				</Button>
			</div>
		</div>
	);
}
