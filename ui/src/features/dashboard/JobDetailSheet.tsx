import { Pause, Play, RotateCw, Trash2 } from "lucide-react";
import { useEffect, useState } from "react";
import { TitleBackdrop, useTitleName } from "@/artwork/TitleBackdrop";
import { Badge } from "@/components/ui/badge";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Progress } from "@/components/ui/progress";
import {
	Sheet,
	SheetContent,
	SheetDescription,
	SheetHeader,
	SheetTitle,
} from "@/components/ui/sheet";
import type { DashboardActions, JobDetail } from "@/data/types";
import { useMediaQuery } from "@/hooks/useMediaQuery";
import { formatBytes, formatPercent, formatSpeed } from "@/lib/format";
import { jobEta, jobKindLabel, jobStatusLine } from "./jobText";
import { StageStepper } from "./StageStepper";
import { TitleArt } from "./TitleArt";

type JobDetailSheetProps = {
	jobId: string | null;
	actions: DashboardActions;
	onClose: () => void;
};

function withKeys<T extends { label: string; detail: string }>(
	items: T[],
): { key: string; item: T }[] {
	const seen = new Map<string, number>();
	return items.map((item) => {
		const base = `${item.label}|${item.detail}`;
		const count = seen.get(base) ?? 0;
		seen.set(base, count + 1);
		return { key: `${base}|${count}`, item };
	});
}

function Section({
	title,
	children,
}: {
	title: string;
	children: React.ReactNode;
}) {
	return (
		<section className="flex flex-col gap-3">
			<h3 className="text-muted-foreground text-xs">{title}</h3>
			{children}
		</section>
	);
}

function SpeedLimit({
	detail,
	actions,
}: {
	detail: JobDetail;
	actions: DashboardActions;
}) {
	const [value, setValue] = useState(
		detail.maxDownloadLimit ? String(detail.maxDownloadLimit / 1_000_000) : "",
	);
	const apply = () =>
		actions.setSpeedLimit(detail.job.id, (Number(value) || 0) * 1_000_000);
	return (
		<form
			className="flex items-center gap-2"
			onSubmit={(event) => {
				event.preventDefault();
				apply();
			}}
		>
			<Input
				inputMode="decimal"
				value={value}
				onChange={(event) =>
					setValue(event.target.value.replace(/[^0-9.]/g, ""))
				}
				placeholder="Unlimited"
				aria-label="Download speed limit in MB/s"
				className="max-w-40"
			/>
			<span className="text-muted-foreground text-xs">MB/s</span>
			<Button type="submit" size="sm" variant="secondary" className="ml-auto">
				Apply
			</Button>
		</form>
	);
}

/** Per-download detail: overview, files, connections or peers, and editable options. Refreshes every second while open. */
export function JobDetailSheet({
	jobId,
	actions,
	onClose,
}: JobDetailSheetProps) {
	const [detail, setDetail] = useState<JobDetail | null>(null);
	const desktop = useMediaQuery("(min-width: 768px)");

	useEffect(() => {
		if (!jobId) {
			setDetail(null);
			return;
		}
		let cancelled = false;
		const load = () =>
			actions.loadDetail(jobId).then((next) => {
				if (!cancelled) {
					setDetail(next);
				}
			});
		void load();
		const timer = setInterval(() => {
			if (document.visibilityState === "visible") {
				void load();
			}
		}, 1000);
		return () => {
			cancelled = true;
			clearInterval(timer);
		};
	}, [jobId, actions]);

	const rawJob = detail?.job;
	const displayName = useTitleName(rawJob?.titleId ?? "", rawJob?.name ?? "");
	const job = rawJob && { ...rawJob, name: displayName };
	return (
		<Sheet open={jobId !== null} onOpenChange={(open) => !open && onClose()}>
			<SheetContent
				side={desktop ? "right" : "bottom"}
				className="max-h-[92dvh] gap-0 overflow-y-auto rounded-t-3xl p-0 data-[side=right]:w-full data-[side=right]:rounded-l-3xl data-[side=right]:rounded-tr-none data-[side=right]:sm:max-w-md"
			>
				{job && detail ? (
					<div className="flex flex-col gap-7 p-6">
						<div className="relative isolate -mx-6 -mt-6 overflow-hidden px-6 pt-24 pb-1">
							<TitleBackdrop titleId={job.titleId} className="-z-10" />
							<SheetHeader className="flex-row items-center gap-4 p-0 pr-8">
								<TitleArt
									titleId={job.titleId}
									name={job.name}
									showTitleId={false}
									className="size-16 rounded-2xl text-[10px]"
								/>
								<div className="min-w-0">
									<SheetTitle className="truncate text-xl font-normal">
										{job.name}
									</SheetTitle>
									<SheetDescription className="truncate font-mono text-xs">
										{job.sourceName}
									</SheetDescription>
								</div>
							</SheetHeader>
						</div>
						<Section title="Overview">
							<div className="flex items-center justify-between gap-3">
								<p className="text-[13px]">{job.error ?? jobStatusLine(job)}</p>
								<Badge
									variant="outline"
									className="shrink-0 rounded-full font-mono text-[10px] tracking-wider"
								>
									{jobKindLabel(job)}
								</Badge>
							</div>
							<Progress
								value={
									job.stageBytes > 0
										? (job.stageDoneBytes / job.stageBytes) * 100
										: 0
								}
								className="h-1.5"
							/>
							<StageStepper
								kind={job.kind}
								stage={job.stage}
								status={job.status}
							/>
							<dl className="grid grid-cols-2 gap-x-6 gap-y-3 text-[13px]">
								<div>
									<dt className="text-muted-foreground text-xs">Downloaded</dt>
									<dd>
										{formatBytes(job.stageDoneBytes)} of{" "}
										{formatBytes(job.stageBytes)}
									</dd>
								</div>
								<div>
									<dt className="text-muted-foreground text-xs">Progress</dt>
									<dd>{formatPercent(job.stageDoneBytes, job.stageBytes)}</dd>
								</div>
								<div>
									<dt className="text-muted-foreground text-xs">Speed</dt>
									<dd>{formatSpeed(job.speed)}</dd>
								</div>
								<div>
									<dt className="text-muted-foreground text-xs">Remaining</dt>
									<dd>{jobEta(job)}</dd>
								</div>
								<div className="col-span-2">
									<dt className="text-muted-foreground text-xs">Installs as</dt>
									<dd className="font-mono text-xs">{job.installName}</dd>
								</div>
								<div className="col-span-2">
									<dt className="text-muted-foreground text-xs">Saved in</dt>
									<dd className="truncate font-mono text-xs">
										{detail.directory}
									</dd>
								</div>
							</dl>
							<div className="flex flex-wrap gap-2">
								{(job.status === "active" ||
									job.status === "paused" ||
									job.status === "waiting") && (
									<Button
										variant="secondary"
										size="sm"
										onClick={() => actions.togglePause(job.id)}
									>
										{job.status === "active" ? (
											<Pause strokeWidth={1.5} />
										) : (
											<Play strokeWidth={1.5} />
										)}
										{job.status === "active" ? "Pause" : "Resume"}
									</Button>
								)}
								{job.status === "failed" && (
									<Button
										variant="secondary"
										size="sm"
										onClick={() => actions.retry(job.id)}
									>
										<RotateCw strokeWidth={1.5} />
										Retry
									</Button>
								)}
								<Button
									variant="destructive"
									size="sm"
									onClick={() => {
										actions.remove(job.id);
										onClose();
									}}
								>
									<Trash2 strokeWidth={1.5} />
									Remove
								</Button>
							</div>
						</Section>
						<Section title={`Files · ${detail.files.length}`}>
							<ul className="flex flex-col gap-3">
								{detail.files.map((file) => (
									<li key={file.index} className="flex flex-col gap-1.5">
										<div className="flex justify-between gap-3 text-[13px]">
											<span
												className={
													file.selected
														? "truncate"
														: "text-muted-foreground truncate line-through"
												}
											>
												{file.path.split("/").pop() || file.path}
											</span>
											<span className="text-muted-foreground shrink-0 text-xs">
												{formatBytes(file.length)}
											</span>
										</div>
										<Progress
											value={
												file.length > 0
													? (file.completed / file.length) * 100
													: 0
											}
										/>
									</li>
								))}
							</ul>
						</Section>
						<Section
							title={`${detail.connectionKind === "peers" ? "Peers" : "Connections"} · ${detail.connections.length}`}
						>
							{detail.connections.length > 0 ? (
								<ul className="flex flex-col divide-y divide-border">
									{withKeys(detail.connections).map(
										({ key, item: connection }) => (
											<li
												key={key}
												className="flex items-center justify-between gap-3 py-2 text-[13px]"
											>
												<div className="min-w-0">
													<p className="truncate">{connection.label}</p>
													<p className="text-muted-foreground truncate text-xs">
														{connection.detail}
													</p>
												</div>
												<span className="shrink-0 text-xs">
													{formatSpeed(connection.downloadSpeed)}
												</span>
											</li>
										),
									)}
								</ul>
							) : (
								<p className="text-muted-foreground text-[13px]">
									No open connections.
								</p>
							)}
						</Section>
						{job.status !== "done" && job.status !== "failed" && (
							<Section title="Speed limit">
								<SpeedLimit key={job.id} detail={detail} actions={actions} />
							</Section>
						)}
					</div>
				) : (
					<div className="flex flex-col gap-4 p-6">
						<SheetHeader className="p-0">
							<SheetTitle className="text-xl font-normal">Download</SheetTitle>
							<SheetDescription>Loading…</SheetDescription>
						</SheetHeader>
						<div className="bg-foreground/[0.06] h-16 animate-pulse rounded-2xl" />
						<div className="bg-foreground/[0.06] h-32 animate-pulse rounded-2xl" />
					</div>
				)}
			</SheetContent>
		</Sheet>
	);
}
