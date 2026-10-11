import { cn } from "cn";
import { useState } from "react";
import type { Dashboard } from "@/data/types";
import { formatBytes, splitSpeed } from "@/lib/format";
import { SpeedChart } from "./SpeedChart";

type ThroughputCardProps = {
	dashboard: Dashboard;
};

type Metric = "speed" | "peak" | "today";

/** Vercel-style metric tabs over a live 60-second throughput chart. */
export function ThroughputCard({ dashboard }: ThroughputCardProps) {
	const [metric, setMetric] = useState<Metric>("speed");
	const history = dashboard.speedHistory;
	const current = history[history.length - 1] ?? 0;
	const peak = Math.max(0, ...history);
	const minute = history.reduce((sum, value) => sum + value, 0);
	const metrics: { id: Metric; label: string; value: string; unit: string }[] =
		[
			{ id: "speed", label: "Speed", ...splitSpeed(current) },
			{ id: "peak", label: "Peak · 60s", ...splitSpeed(peak) },
			(() => {
				const [value, unit] = formatBytes(minute).split(" ");
				return { id: "today" as const, label: "Last minute", value, unit };
			})(),
		];
	return (
		<section className="surface flex flex-col overflow-hidden">
			<div
				className="grid grid-cols-3 border-b border-border"
				role="tablist"
				aria-label="Throughput"
			>
				{metrics.map((item) => (
					<button
						key={item.id}
						type="button"
						role="tab"
						aria-selected={metric === item.id}
						onClick={() => setMetric(item.id)}
						className={cn(
							"relative flex flex-col items-start gap-1 px-4 py-4 text-left transition-colors md:px-6",
							"focus-visible:ring-ring outline-none focus-visible:ring-2 focus-visible:ring-inset",
							metric === item.id
								? "bg-foreground/[0.04]"
								: "hover:bg-foreground/[0.02]",
						)}
					>
						<span className="text-muted-foreground text-xs">{item.label}</span>
						<span className="text-xl font-light tracking-tight md:text-2xl">
							{item.value}
							<span className="text-muted-foreground ml-1 text-xs">
								{item.unit}
							</span>
						</span>
						{metric === item.id && (
							<span className="bg-foreground absolute inset-x-0 bottom-0 h-px" />
						)}
					</button>
				))}
			</div>
			<div className="relative px-2 pt-6 pb-2">
				<SpeedChart samples={history} className="text-foreground h-40 w-full" />
				<div className="text-muted-foreground flex justify-between px-2 pt-2 text-[11px]">
					<span>60s ago</span>
					<span>now</span>
				</div>
			</div>
		</section>
	);
}
