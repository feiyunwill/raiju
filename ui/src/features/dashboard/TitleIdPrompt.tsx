import { useState } from "react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import type { Job } from "@/data/types";

type TitleIdPromptProps = {
	job: Job;
	onSubmit: (id: string, titleId: string) => void;
};

const titleIdPattern = /^[A-Z]{4}\d{5}$/;

/** Inline prompt for a dump whose name and `param.json` carried no title ID: the ID names it. */
export function TitleIdPrompt({ job, onSubmit }: TitleIdPromptProps) {
	const [titleId, setTitleId] = useState("");
	const ready = titleIdPattern.test(titleId);
	return (
		<div className="bg-foreground/[0.06] flex flex-wrap items-center gap-x-3 gap-y-2 rounded-2xl px-3 py-2 text-[13px]">
			<span className="min-w-0 flex-1 truncate">
				No title ID in this dump — name it to install
			</span>
			<div className="flex items-center gap-1">
				<Input
					value={titleId}
					onChange={(event) =>
						setTitleId(
							event.target.value
								.toUpperCase()
								.replace(/[^A-Z0-9]/g, "")
								.slice(0, 9),
						)
					}
					onClick={(event) => event.stopPropagation()}
					onKeyDown={(event) => {
						event.stopPropagation();
						if (event.key === "Enter" && ready) {
							onSubmit(job.id, titleId);
						}
					}}
					aria-label="Title ID"
					placeholder="PPSA12345"
					className="w-32 rounded-full text-center font-mono"
				/>
				<Button
					size="sm"
					variant="secondary"
					disabled={!ready}
					onClick={(event) => {
						event.stopPropagation();
						onSubmit(job.id, titleId);
					}}
				>
					Use
				</Button>
			</div>
		</div>
	);
}
