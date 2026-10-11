import { Pause, Play } from "lucide-react";
import { TitleBackdrop, useTitleName } from "@/artwork/TitleBackdrop";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Progress } from "@/components/ui/progress";
import type { Job } from "@/data/types";
import { formatBytes, formatPercent, splitSpeed } from "@/lib/format";
import { jobEta, jobKindLabel } from "./jobText";
import { StageStepper } from "./StageStepper";
import { TitleArt } from "./TitleArt";

type NowDownloadingProps = {
	job: Job | undefined;
	onTogglePause: (id: string) => void;
	onAdd: () => void;
};

/** Hero tile for the job currently moving through the pipeline. */
export function NowDownloading({
	job,
	onTogglePause,
	onAdd,
}: NowDownloadingProps) {
	if (!job) {
		return (
			<section className="surface flex flex-col items-start gap-5 p-6 md:p-8">
				<p className="text-muted-foreground text-[13px]">Now downloading</p>
				<h2 className="text-2xl font-light md:text-[28px]">
					Your queue is clear
				</h2>
				<p className="text-muted-foreground max-w-md">
					Paste a link and Raiju downloads, verifies and installs it straight
					onto your PS5.
				</p>
				<Button onClick={onAdd}>Add download</Button>
			</section>
		);
	}
	return <ActiveJob job={job} onTogglePause={onTogglePause} />;
}

function ActiveJob({
	job,
	onTogglePause,
}: {
	job: Job;
	onTogglePause: (id: string) => void;
}) {
	const speed = splitSpeed(job.speed);
	const active = job.status === "active";
	const name = useTitleName(job.titleId, job.name);
	const knownSize = job.stageBytes > 0;
	return (
		<section className="surface relative isolate flex flex-col gap-6 overflow-hidden p-5 md:flex-row md:gap-8 md:p-8">
			<TitleBackdrop titleId={job.titleId} className="-z-10" />
			<TitleArt
				titleId={job.titleId}
				name={name}
				className="aspect-square w-32 self-start text-[22px] shadow-2xl md:w-56 md:text-[28px]"
			/>
			<div className="flex min-w-0 flex-1 flex-col gap-6">
				<div className="flex items-start justify-between gap-4">
					<div className="min-w-0">
						<p className="text-muted-foreground text-[13px]">
							Now {active ? "downloading" : "paused"}
						</p>
						<h2 className="truncate text-2xl font-normal tracking-tight md:text-[28px]">
							{name}
						</h2>
						<p className="text-muted-foreground mt-1 truncate font-mono text-xs">
							{job.sourceName}
						</p>
					</div>
					<Badge
						variant="outline"
						className="shrink-0 rounded-full font-mono text-[10px] tracking-wider"
					>
						{jobKindLabel(job)}
					</Badge>
				</div>
				<div className="flex flex-wrap items-end gap-x-10 gap-y-4">
					<div>
						<p className="text-6xl leading-none font-light tracking-tighter md:text-7xl">
							{speed.value}
							<span className="text-muted-foreground ml-2 text-2xl font-light tracking-normal">
								{speed.unit}
							</span>
						</p>
					</div>
					<dl className="grid grid-cols-3 gap-x-8 gap-y-1 text-[13px]">
						<dt className="text-muted-foreground">Progress</dt>
						<dt className="text-muted-foreground">Remaining</dt>
						<dt className="text-muted-foreground">Connections</dt>
						<dd>{formatPercent(job.stageDoneBytes, job.stageBytes)}</dd>
						<dd>{jobEta(job)}</dd>
						<dd>{job.connections || "—"}</dd>
					</dl>
				</div>
				<div className="flex flex-col gap-2">
					{knownSize ? (
						<Progress
							value={(job.stageDoneBytes / job.stageBytes) * 100}
							className="h-1.5"
						/>
					) : (
						<div className="bg-muted relative h-1.5 overflow-hidden rounded-full">
							<span className="bg-primary/70 absolute inset-y-0 w-1/3 animate-[raiju-indeterminate_1.4s_ease-in-out_infinite] rounded-full" />
						</div>
					)}
					<div className="text-muted-foreground flex justify-between text-xs">
						<span>
							{knownSize
								? `${formatBytes(job.stageDoneBytes)} of ${formatBytes(job.stageBytes)}`
								: `${formatBytes(job.stageDoneBytes)} · size unknown`}
						</span>
						<span className="font-mono">→ {job.installName}</span>
					</div>
				</div>
				<div className="flex flex-col-reverse gap-5 sm:flex-row sm:items-center">
					<Button
						variant={active ? "secondary" : "default"}
						onClick={() => onTogglePause(job.id)}
					>
						{active ? <Pause strokeWidth={1.5} /> : <Play strokeWidth={1.5} />}
						{active ? "Pause" : "Resume"}
					</Button>
					<div className="flex-1">
						<StageStepper
							kind={job.kind}
							stage={job.stage}
							status={job.status}
						/>
					</div>
				</div>
			</div>
		</section>
	);
}
