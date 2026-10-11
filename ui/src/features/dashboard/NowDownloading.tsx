import { Pause, Play } from "lucide-react";
import { TitleBackdrop, useTitleName } from "@/artwork/TitleBackdrop";
import { Button } from "@/components/ui/button";
import type { DashboardActions, Job } from "@/data/types";
import { formatBytes } from "@/lib/format";
import { ConflictPrompt } from "./ConflictPrompt";
import { JobKindBadge } from "./JobKindBadge";
import {
	jobEta,
	jobHasConflict,
	jobNeedsTitleId,
	jobStageFigure,
	jobStageVerb,
} from "./jobText";
import { StageProgress } from "./StageProgress";
import { StageStepper } from "./StageStepper";
import { TitleArt } from "./TitleArt";

type NowDownloadingProps = {
	job: Job | undefined;
	actions: DashboardActions;
	onAdd: () => void;
};

/** Hero tile for the job currently moving through the pipeline. */
export function NowDownloading({ job, actions, onAdd }: NowDownloadingProps) {
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
	return <ActiveJob job={job} actions={actions} />;
}

function ActiveJob({ job, actions }: { job: Job; actions: DashboardActions }) {
	const paused = job.status === "paused";
	const active = job.status === "active";
	const waiting = jobHasConflict(job) || jobNeedsTitleId(job);
	const name = useTitleName(job.titleId, job.name);
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
							{paused
								? `Paused while ${jobStageVerb(job)}`
								: `Now ${jobStageVerb(job)}`}
						</p>
						<h2 className="truncate text-2xl font-normal tracking-tight md:text-[28px]">
							{name}
						</h2>
						<p className="text-muted-foreground mt-1 truncate font-mono text-xs">
							{job.sourceName}
						</p>
					</div>
					<JobKindBadge job={job} />
				</div>
				<div className="flex flex-wrap items-end gap-x-10 gap-y-4">
					<div>
						<p className="text-6xl leading-none font-light tracking-tighter md:text-7xl">
							<span className="tabular-nums">
								{formatBytes(job.speed).split(" ")[0]}
							</span>
							<span className="text-muted-foreground ml-2 text-2xl font-light tracking-normal">
								{formatBytes(job.speed).split(" ")[1]}/s
							</span>
						</p>
					</div>
					<dl className="grid grid-cols-3 gap-x-8 gap-y-1 text-[13px]">
						<dt className="text-muted-foreground">Progress</dt>
						<dt className="text-muted-foreground">Remaining</dt>
						<dt className="text-muted-foreground">Connections</dt>
						<dd>{jobStageFigure(job)}</dd>
						<dd>{jobEta(job)}</dd>
						<dd>{job.connections || "—"}</dd>
					</dl>
				</div>
				<div className="flex flex-col gap-2">
					<StageProgress job={job} />
					<div className="text-muted-foreground flex justify-between text-xs">
						<span>
							{job.stageBytes > 0
								? `${formatBytes(job.stageDoneBytes)} of ${formatBytes(job.stageBytes)} for this stage`
								: `${formatBytes(job.stageDoneBytes)} · size unknown`}
						</span>
						<span className="font-mono">→ {job.installName}</span>
					</div>
				</div>
				{jobHasConflict(job) && (
					<ConflictPrompt job={job} onResolve={actions.resolveConflict} />
				)}
				{jobNeedsTitleId(job) && (
					<p className="text-muted-foreground text-[13px]">
						Rename the file after adding a{" "}
						<span className="font-mono">PPSA#####</span> or{" "}
						<span className="font-mono">CUSA#####</span> title ID, or add the
						link again with one.
					</p>
				)}
				<div className="flex flex-col-reverse gap-5 sm:flex-row sm:items-center">
					{!waiting && (
						<Button
							variant={active ? "secondary" : "default"}
							onClick={() => actions.togglePause(job.id)}
						>
							{active ? (
								<Pause strokeWidth={1.5} />
							) : (
								<Play strokeWidth={1.5} />
							)}
							{active ? "Pause" : "Resume"}
						</Button>
					)}
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
