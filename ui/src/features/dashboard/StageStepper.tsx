import { cn } from "cn";
import { Check } from "lucide-react";
import {
	type JobKind,
	type JobStatus,
	type Stage,
	stagesFor,
} from "@/data/types";

const labels: Record<Stage, string> = {
	download: "Download",
	verify: "Verify",
	extract: "Extract",
	install: "Install",
	ready: "Ready",
};

type StageStepperProps = {
	kind: JobKind;
	stage: Stage;
	status: JobStatus;
	compact?: boolean;
};

/** Pipeline progress: completed steps filled, current step ringed, upcoming steps outlined. */
export function StageStepper({
	kind,
	stage,
	status,
	compact = false,
}: StageStepperProps) {
	const stages = stagesFor[kind];
	const current = stages.indexOf(stage);
	const failed = status === "failed";
	return (
		<ol className="flex w-full items-center" aria-label="Pipeline stages">
			{stages.map((step, i) => {
				const done = i < current || (step === "ready" && stage === "ready");
				const active = i === current && stage !== "ready";
				return (
					<li
						key={step}
						className={cn(
							"flex items-center",
							i < stages.length - 1 && "flex-1",
						)}
					>
						<div className="flex flex-col items-center gap-1.5">
							<span
								aria-current={active ? "step" : undefined}
								className={cn(
									"flex size-4 items-center justify-center rounded-full border transition-colors duration-300",
									done && "border-foreground bg-foreground text-background",
									active &&
										!failed &&
										"border-foreground ring-4 ring-foreground/15",
									active && failed && "border-destructive bg-destructive/20",
									!done && !active && "border-foreground/25",
								)}
							>
								{done && <Check className="size-2.5" strokeWidth={3} />}
								{active && !failed && status === "active" && (
									<span className="size-1.5 animate-pulse rounded-full bg-foreground" />
								)}
							</span>
							{!compact && (
								<span
									className={cn(
										"text-[11px] leading-none",
										done || active
											? "text-foreground"
											: "text-muted-foreground",
										active && failed && "text-destructive",
									)}
								>
									{labels[step]}
								</span>
							)}
						</div>
						{i < stages.length - 1 && (
							<span
								className={cn(
									"mx-1.5 h-px flex-1 transition-colors duration-300",
									!compact && "-mt-[17px]",
									i < current ? "bg-foreground" : "bg-foreground/15",
								)}
							/>
						)}
					</li>
				);
			})}
		</ol>
	);
}
