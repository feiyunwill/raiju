import { cn } from "cn";
import { Pause, Play, RotateCw } from "lucide-react";
import { useState } from "react";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import type { Job, JobStatus } from "@/data/types";
import type { DashboardActions } from "@/data/useDashboard";
import { formatBytes } from "@/lib/format";
import { jobEta, jobKindLabel, jobStatusLine } from "./jobText";
import { StageStepper } from "./StageStepper";
import { TitleArt } from "./TitleArt";

type Tab = "active" | "waiting" | "done" | "failed";

const tabs: { id: Tab; label: string; statuses: JobStatus[] }[] = [
	{ id: "active", label: "Active", statuses: ["active", "paused"] },
	{ id: "waiting", label: "Waiting", statuses: ["waiting"] },
	{ id: "done", label: "Done", statuses: ["done"] },
	{ id: "failed", label: "Failed", statuses: ["failed"] },
];

type QueueListProps = {
	jobs: Job[];
	actions: DashboardActions;
	limit?: number;
};

function JobRow({ job, actions }: { job: Job; actions: DashboardActions }) {
	const canToggle =
		job.status === "active" ||
		job.status === "paused" ||
		job.status === "waiting";
	return (
		<li className="group flex items-center gap-4 rounded-2xl px-3 py-3 transition-colors hover:bg-foreground/[0.03] md:px-4">
			<TitleArt
				titleId={job.titleId}
				name={job.name}
				showTitleId={false}
				className="size-14 rounded-xl text-[9px]"
			/>
			<div className="flex min-w-0 flex-1 flex-col gap-2">
				<div className="flex items-baseline justify-between gap-3">
					<p className="truncate font-medium">{job.name}</p>
					<span className="text-muted-foreground hidden shrink-0 font-mono text-[10px] tracking-wider sm:inline">
						{jobKindLabel(job)}
					</span>
				</div>
				{job.status === "active" || job.status === "paused" ? (
					<Progress value={(job.stageDoneBytes / job.stageBytes) * 100} />
				) : (
					<div className="max-w-xs">
						<StageStepper
							kind={job.kind}
							stage={job.stage}
							status={job.status}
							compact
						/>
					</div>
				)}
				<p
					className={cn(
						"truncate text-xs",
						job.status === "failed"
							? "text-destructive"
							: "text-muted-foreground",
					)}
				>
					{job.error ?? jobStatusLine(job)}
				</p>
			</div>
			<div className="text-muted-foreground hidden w-24 shrink-0 text-right text-xs md:block">
				<p className="text-foreground">{formatBytes(job.totalBytes)}</p>
				<p>{jobEta(job)}</p>
			</div>
			{canToggle && (
				<Button
					size="icon-sm"
					variant="secondary"
					aria-label={
						job.status === "active" ? `Pause ${job.name}` : `Resume ${job.name}`
					}
					onClick={() => actions.togglePause(job.id)}
				>
					{job.status === "active" ? (
						<Pause strokeWidth={1.5} />
					) : (
						<Play strokeWidth={1.5} />
					)}
				</Button>
			)}
			{job.status === "failed" && (
				<Button
					size="icon-sm"
					variant="secondary"
					aria-label={`Retry ${job.name}`}
					onClick={() => actions.retry(job.id)}
				>
					<RotateCw strokeWidth={1.5} />
				</Button>
			)}
		</li>
	);
}

/** Queue grouped into Active, Waiting, Done and Failed pill tabs with counts. */
export function QueueList({ jobs, actions, limit }: QueueListProps) {
	const [tab, setTab] = useState<Tab>("active");
	const current = tabs.find((item) => item.id === tab) ?? tabs[0];
	const visible = jobs.filter((job) => current.statuses.includes(job.status));
	const shown = limit ? visible.slice(0, limit) : visible;
	return (
		<section className="surface flex flex-col gap-3 p-3 md:p-4">
			<div className="flex flex-wrap items-center justify-between gap-3 px-1 pt-1">
				<div
					className="flex gap-1 overflow-x-auto"
					role="tablist"
					aria-label="Queue"
				>
					{tabs.map((item) => {
						const count = jobs.filter((job) =>
							item.statuses.includes(job.status),
						).length;
						return (
							<button
								key={item.id}
								type="button"
								role="tab"
								aria-selected={tab === item.id}
								onClick={() => setTab(item.id)}
								className={cn(
									"flex h-9 items-center gap-2 rounded-full px-4 text-[13px] whitespace-nowrap transition-colors duration-150",
									"focus-visible:ring-ring focus-visible:ring-offset-background outline-none focus-visible:ring-2 focus-visible:ring-offset-2",
									tab === item.id
										? "bg-primary text-primary-foreground"
										: "text-muted-foreground hover:bg-foreground/[0.05] hover:text-foreground",
								)}
							>
								{item.label}
								<span
									className={cn(
										"rounded-full px-1.5 text-[11px]",
										tab === item.id
											? "bg-primary-foreground/15"
											: "bg-foreground/[0.08]",
									)}
								>
									{count}
								</span>
							</button>
						);
					})}
				</div>
				{tab === "active" && visible.length > 0 && (
					<div className="flex gap-1">
						<Button size="sm" variant="ghost" onClick={actions.pauseAll}>
							Pause all
						</Button>
						<Button size="sm" variant="ghost" onClick={actions.resumeAll}>
							Resume all
						</Button>
					</div>
				)}
			</div>
			{shown.length > 0 ? (
				<ul className="flex flex-col">
					{shown.map((job) => (
						<JobRow key={job.id} job={job} actions={actions} />
					))}
				</ul>
			) : (
				<p className="text-muted-foreground px-4 py-10 text-center text-[13px]">
					Nothing {current.label.toLowerCase()}.
				</p>
			)}
		</section>
	);
}
