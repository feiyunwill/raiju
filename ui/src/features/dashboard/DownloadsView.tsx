import type { DashboardActions, Job } from "@/data/types";
import { QueueList } from "./QueueList";

type DownloadsViewProps = {
	jobs: Job[];
	actions: DashboardActions;
	onOpenDetail: (id: string) => void;
};

/** Full download queue. */
export function DownloadsView({
	jobs,
	actions,
	onOpenDetail,
}: DownloadsViewProps) {
	return (
		<div className="flex flex-col gap-6">
			<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
				Downloads
			</h1>
			<QueueList jobs={jobs} actions={actions} onOpenDetail={onOpenDetail} />
		</div>
	);
}
