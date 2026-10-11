import type { Dashboard, DashboardActions } from "@/data/types";
import { formatDuration } from "@/lib/format";
import { jobHasConflict, jobNeedsTitleId } from "./jobText";
import { NowDownloading } from "./NowDownloading";
import { QueueList } from "./QueueList";
import { StorageCard } from "./StorageCard";
import { ThroughputCard } from "./ThroughputCard";

type DashboardViewProps = {
	dashboard: Dashboard;
	actions: DashboardActions;
	onAdd: () => void;
	onViewAll: () => void;
	onOpenDetail: (id: string) => void;
};

/** Home: the current job, live throughput, storage and the head of the queue. */
export function DashboardView({
	dashboard,
	actions,
	onAdd,
	onViewAll,
	onOpenDetail,
}: DashboardViewProps) {
	const current =
		dashboard.jobs.find((job) => job.status === "active") ??
		dashboard.jobs.find(jobHasConflict) ??
		dashboard.jobs.find(jobNeedsTitleId) ??
		dashboard.jobs.find((job) => job.status === "paused");
	const incoming = dashboard.jobs
		.filter(
			(job) =>
				job.status === "waiting" ||
				job.status === "paused" ||
				job.status === "active",
		)
		.reduce(
			(sum, job) =>
				sum +
				Math.max(
					0,
					job.totalBytes -
						(job.stage === "download" ? job.stageDoneBytes : job.totalBytes),
				),
			0,
		);
	const { version, uptimeSeconds } = dashboard.console;
	return (
		<div className="flex flex-col gap-6">
			<div className="flex flex-col gap-1">
				<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
					Overview
				</h1>
				<p className="text-muted-foreground text-[13px]">
					{version || "Connecting…"}
					{uptimeSeconds !== null && ` · up ${formatDuration(uptimeSeconds)}`}
				</p>
			</div>
			<NowDownloading job={current} actions={actions} onAdd={onAdd} />
			<div className="grid gap-6 lg:grid-cols-3">
				<div className="lg:col-span-2">
					<ThroughputCard dashboard={dashboard} />
				</div>
				<StorageCard storage={dashboard.storage} incomingBytes={incoming} />
			</div>
			<div className="flex flex-col gap-3">
				<div className="flex items-center justify-between px-1">
					<h2 className="text-lg font-medium">Queue</h2>
					<button
						type="button"
						onClick={onViewAll}
						className="text-muted-foreground hover:text-foreground focus-visible:ring-ring rounded-full px-2 text-[13px] outline-none focus-visible:ring-2"
					>
						View all
					</button>
				</div>
				<QueueList
					jobs={dashboard.jobs}
					actions={actions}
					limit={4}
					onOpenDetail={onOpenDetail}
				/>
			</div>
		</div>
	);
}
