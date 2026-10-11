import type { Console } from "@/data/types";
import { formatDuration } from "@/lib/format";
import { ThemeToggle } from "@/theme/ThemeToggle";

function Row({
	label,
	children,
}: {
	label: string;
	children: React.ReactNode;
}) {
	return (
		<div className="flex min-h-14 items-center justify-between gap-4 px-5 py-3">
			<span className="text-[15px]">{label}</span>
			<span className="text-muted-foreground text-[13px]">{children}</span>
		</div>
	);
}

/** Appearance and console information. */
export function SettingsView({ console }: { console: Console }) {
	return (
		<div className="flex max-w-2xl flex-col gap-6">
			<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
				Settings
			</h1>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Appearance</h2>
				<div className="surface divide-border divide-y">
					<Row label="Theme">
						<ThemeToggle />
					</Row>
				</div>
			</section>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Console</h2>
				<div className="surface divide-border divide-y">
					<Row label="Address">
						<span className="font-mono">{console.host}</span>
					</Row>
					<Row label="Raiju version">{console.version}</Row>
					<Row label="Uptime">{formatDuration(console.uptimeSeconds)}</Row>
				</div>
			</section>
		</div>
	);
}
