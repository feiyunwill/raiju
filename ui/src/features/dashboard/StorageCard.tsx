import { HardDrive } from "lucide-react";
import type { Storage } from "@/data/types";
import { formatBytes } from "@/lib/format";

type StorageCardProps = {
	storage: Storage | null;
	incomingBytes: number;
};

/** Internal NVMe usage split into installed games, Raiju staging and free space. */
export function StorageCard({ storage, incomingBytes }: StorageCardProps) {
	if (!storage) {
		return (
			<section className="surface flex flex-col gap-5 p-5 md:p-6">
				<div className="flex items-center justify-between">
					<p className="text-muted-foreground text-[13px]">Internal storage</p>
					<HardDrive
						className="text-muted-foreground size-4"
						strokeWidth={1.5}
					/>
				</div>
				<div className="bg-foreground/10 h-2 w-full rounded-full" />
				<p className="text-muted-foreground text-[13px]">
					Free space shows up once Raiju's console API is connected.
				</p>
				{incomingBytes > 0 && (
					<p className="bg-foreground/[0.05] rounded-xl px-3 py-2 text-xs">
						Queue needs {formatBytes(incomingBytes)}
					</p>
				)}
			</section>
		);
	}
	const free = Math.max(
		0,
		storage.totalBytes - storage.gamesBytes - storage.stagingBytes,
	);
	const fits = incomingBytes <= free;
	const segments = [
		{ label: "Games", bytes: storage.gamesBytes, className: "bg-foreground" },
		{
			label: "Raiju staging",
			bytes: storage.stagingBytes,
			className: "bg-foreground/45",
		},
	];
	return (
		<section className="surface flex flex-col gap-5 p-5 md:p-6">
			<div className="flex items-center justify-between">
				<p className="text-muted-foreground text-[13px]">Internal storage</p>
				<HardDrive className="text-muted-foreground size-4" strokeWidth={1.5} />
			</div>
			<p className="text-4xl leading-none font-light tracking-tight">
				{formatBytes(free).split(" ")[0]}
				<span className="text-muted-foreground ml-1.5 text-base">
					{formatBytes(free).split(" ")[1]} free
				</span>
			</p>
			<div
				className="bg-foreground/10 flex h-2 w-full overflow-hidden rounded-full"
				role="img"
				aria-label={`${formatBytes(free)} free of ${formatBytes(storage.totalBytes)}`}
			>
				{segments.map((segment) => (
					<span
						key={segment.label}
						className={`${segment.className} h-full transition-[width] duration-1000 ease-linear first:rounded-l-full`}
						style={{ width: `${(segment.bytes / storage.totalBytes) * 100}%` }}
					/>
				))}
			</div>
			<ul className="flex flex-col gap-2 text-[13px]">
				{segments.map((segment) => (
					<li key={segment.label} className="flex items-center gap-2">
						<span className={`${segment.className} size-2 rounded-full`} />
						<span className="text-muted-foreground flex-1">
							{segment.label}
						</span>
						<span>{formatBytes(segment.bytes)}</span>
					</li>
				))}
				<li className="flex items-center gap-2">
					<span className="bg-foreground/10 size-2 rounded-full" />
					<span className="text-muted-foreground flex-1">
						Free of {formatBytes(storage.totalBytes)}
					</span>
					<span>{formatBytes(free)}</span>
				</li>
			</ul>
			{incomingBytes > 0 && (
				<p
					className={`rounded-xl px-3 py-2 text-xs ${fits ? "bg-foreground/[0.05]" : "bg-destructive/15 text-destructive"}`}
				>
					Queue needs {formatBytes(incomingBytes)} ·{" "}
					{fits ? "fits" : "not enough space"}
				</p>
			)}
		</section>
	);
}
