import { cn } from "cn";
import {
	Copy,
	Info,
	MoreHorizontal,
	Pause,
	Play,
	RotateCw,
	Trash2,
} from "lucide-react";
import { type KeyboardEvent, useRef, useState } from "react";
import { useTitleName } from "@/artwork/TitleBackdrop";
import { Button } from "@/components/ui/button";
import {
	DropdownMenu,
	DropdownMenuContent,
	DropdownMenuItem,
	DropdownMenuSeparator,
	DropdownMenuTrigger,
} from "@/components/ui/dropdown-menu";
import { Progress } from "@/components/ui/progress";
import type { DashboardActions, Job, JobStatus } from "@/data/types";
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
	onOpenDetail: (id: string) => void;
	limit?: number;
};

type JobRowProps = {
	job: Job;
	actions: DashboardActions;
	onOpenDetail: (id: string) => void;
	focusable: boolean;
	onFocusRow: () => void;
};

function JobMenu({
	job,
	actions,
	onOpenDetail,
}: Omit<JobRowProps, "focusable" | "onFocusRow">) {
	const canToggle =
		job.status === "active" ||
		job.status === "paused" ||
		job.status === "waiting";
	const link = job.uris?.[0];
	return (
		<DropdownMenu>
			<DropdownMenuTrigger asChild>
				<Button
					size="icon-sm"
					variant="ghost"
					aria-label={`More actions for ${job.name}`}
					onClick={(e) => e.stopPropagation()}
				>
					<MoreHorizontal strokeWidth={1.5} />
				</Button>
			</DropdownMenuTrigger>
			<DropdownMenuContent
				align="end"
				className="min-w-44 rounded-2xl"
				onClick={(e) => e.stopPropagation()}
			>
				<DropdownMenuItem onSelect={() => onOpenDetail(job.id)}>
					<Info strokeWidth={1.5} />
					Details
				</DropdownMenuItem>
				{canToggle && (
					<DropdownMenuItem onSelect={() => actions.togglePause(job.id)}>
						{job.status === "active" ? (
							<Pause strokeWidth={1.5} />
						) : (
							<Play strokeWidth={1.5} />
						)}
						{job.status === "active" ? "Pause" : "Resume"}
					</DropdownMenuItem>
				)}
				{job.status === "failed" && (
					<DropdownMenuItem onSelect={() => actions.retry(job.id)}>
						<RotateCw strokeWidth={1.5} />
						Retry
					</DropdownMenuItem>
				)}
				{link && (
					<DropdownMenuItem
						onSelect={() => void navigator.clipboard?.writeText(link)}
					>
						<Copy strokeWidth={1.5} />
						Copy link
					</DropdownMenuItem>
				)}
				<DropdownMenuSeparator />
				<DropdownMenuItem
					variant="destructive"
					onSelect={() => actions.remove(job.id)}
				>
					<Trash2 strokeWidth={1.5} />
					Remove
				</DropdownMenuItem>
			</DropdownMenuContent>
		</DropdownMenu>
	);
}

function JobRow({
	job: rawJob,
	actions,
	onOpenDetail,
	focusable,
	onFocusRow,
}: JobRowProps) {
	const job = { ...rawJob, name: useTitleName(rawJob.titleId, rawJob.name) };
	const canToggle =
		job.status === "active" ||
		job.status === "paused" ||
		job.status === "waiting";
	return (
		<li
			data-row
			tabIndex={focusable ? 0 : -1}
			onFocus={onFocusRow}
			onClick={() => onOpenDetail(job.id)}
			onKeyDown={(event) => {
				if (
					event.target === event.currentTarget &&
					(event.key === "Enter" || event.key === " ")
				) {
					event.preventDefault();
					onOpenDetail(job.id);
				}
			}}
			aria-label={`${job.name}, ${jobStatusLine(job)}`}
			className={cn(
				"group flex cursor-pointer items-center gap-4 rounded-2xl px-3 py-3 transition-colors outline-none md:px-4",
				"hover:bg-foreground/[0.03] focus-visible:ring-ring focus-visible:ring-2 focus-visible:ring-inset",
			)}
		>
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
					<Progress
						value={
							job.stageBytes > 0
								? (job.stageDoneBytes / job.stageBytes) * 100
								: 0
						}
					/>
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
					onClick={(event) => {
						event.stopPropagation();
						actions.togglePause(job.id);
					}}
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
					onClick={(event) => {
						event.stopPropagation();
						actions.retry(job.id);
					}}
				>
					<RotateCw strokeWidth={1.5} />
				</Button>
			)}
			<JobMenu job={job} actions={actions} onOpenDetail={onOpenDetail} />
		</li>
	);
}

/** Queue grouped into Active, Waiting, Done and Failed pill tabs, with arrow-key navigation between tabs and rows. */
export function QueueList({
	jobs,
	actions,
	onOpenDetail,
	limit,
}: QueueListProps) {
	const [tab, setTab] = useState<Tab>("active");
	const [focusIndex, setFocusIndex] = useState(0);
	const listRef = useRef<HTMLUListElement>(null);
	const tabRefs = useRef<(HTMLButtonElement | null)[]>([]);
	const current = tabs.find((item) => item.id === tab) ?? tabs[0];
	const visible = jobs.filter((job) => current.statuses.includes(job.status));
	const shown = limit ? visible.slice(0, limit) : visible;
	const rovingIndex = Math.min(focusIndex, Math.max(0, shown.length - 1));

	const onTabKey = (event: KeyboardEvent<HTMLButtonElement>, index: number) => {
		const delta =
			event.key === "ArrowRight" ? 1 : event.key === "ArrowLeft" ? -1 : 0;
		if (delta === 0) {
			return;
		}
		event.preventDefault();
		const next = (index + delta + tabs.length) % tabs.length;
		setTab(tabs[next].id);
		tabRefs.current[next]?.focus();
	};

	const onListKey = (event: KeyboardEvent<HTMLUListElement>) => {
		const rows = Array.from(
			listRef.current?.querySelectorAll<HTMLElement>("[data-row]") ?? [],
		);
		const index = rows.indexOf(document.activeElement as HTMLElement);
		if (index < 0) {
			return;
		}
		const keys: Record<string, number> = {
			ArrowDown: index + 1,
			ArrowUp: index - 1,
			Home: 0,
			End: rows.length - 1,
		};
		const next = keys[event.key];
		if (next === undefined) {
			return;
		}
		event.preventDefault();
		const clamped = Math.max(0, Math.min(rows.length - 1, next));
		setFocusIndex(clamped);
		rows[clamped]?.focus();
	};

	return (
		<section className="surface flex flex-col gap-3 p-3 md:p-4">
			<div className="flex flex-wrap items-center justify-between gap-3 px-1 pt-1">
				<div
					className="flex gap-1 overflow-x-auto"
					role="tablist"
					aria-label="Queue"
				>
					{tabs.map((item, index) => {
						const count = jobs.filter((job) =>
							item.statuses.includes(job.status),
						).length;
						const selected = tab === item.id;
						return (
							<button
								key={item.id}
								ref={(node) => {
									tabRefs.current[index] = node;
								}}
								type="button"
								role="tab"
								aria-selected={selected}
								tabIndex={selected ? 0 : -1}
								onClick={() => setTab(item.id)}
								onKeyDown={(event) => onTabKey(event, index)}
								className={cn(
									"flex h-9 items-center gap-2 rounded-full px-4 text-[13px] whitespace-nowrap transition-colors duration-150",
									"focus-visible:ring-ring focus-visible:ring-offset-background outline-none focus-visible:ring-2 focus-visible:ring-offset-2",
									selected
										? "bg-primary text-primary-foreground"
										: "text-muted-foreground hover:bg-foreground/[0.05] hover:text-foreground",
								)}
							>
								{item.label}
								<span
									className={cn(
										"rounded-full px-1.5 text-[11px]",
										selected
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
				<ul
					ref={listRef}
					className="flex flex-col"
					onKeyDown={onListKey}
					aria-label={`${current.label} downloads`}
				>
					{shown.map((job, index) => (
						<JobRow
							key={job.id}
							job={job}
							actions={actions}
							onOpenDetail={onOpenDetail}
							focusable={index === rovingIndex}
							onFocusRow={() => setFocusIndex(index)}
						/>
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
