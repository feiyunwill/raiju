import type { Job } from "@/data/types";
import type { DashboardActions } from "@/data/useDashboard";
import { QueueList } from "./QueueList";

/** Full download queue. */
export function DownloadsView({
	jobs,
	actions,
}: {
	jobs: Job[];
	actions: DashboardActions;
}) {
	return (
		<div className="flex flex-col gap-6">
			<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
				Downloads
			</h1>
			<QueueList jobs={jobs} actions={actions} />
		</div>
	);
}
