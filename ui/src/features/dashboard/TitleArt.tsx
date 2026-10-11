import { cn } from "cn";

type TitleArtProps = {
	titleId: string;
	name: string;
	className?: string;
	showTitleId?: boolean;
};

function hash(text: string): number {
	let value = 0;
	for (const char of text) {
		value = (value * 31 + char.charCodeAt(0)) >>> 0;
	}
	return value;
}

function initials(name: string): string {
	return name
		.split(/\s+/)
		.filter((word) => /^[A-Za-z0-9]/.test(word))
		.slice(0, 2)
		.map((word) => word[0].toUpperCase())
		.join("");
}

/** Monochrome generated cover for a title until real artwork is available from ShadowMountPlus. */
export function TitleArt({
	titleId,
	name,
	className,
	showTitleId = true,
}: TitleArtProps) {
	const seed = hash(titleId);
	const angle = seed % 360;
	const light = 0.22 + (seed % 7) * 0.025;
	return (
		<div
			aria-hidden="true"
			className={cn(
				"relative isolate flex shrink-0 items-end overflow-hidden rounded-2xl ring-1 ring-white/10 select-none",
				className,
			)}
			style={{
				backgroundImage: `radial-gradient(120% 90% at ${20 + (seed % 60)}% 0%, oklch(${light + 0.18} 0 0), transparent 60%), linear-gradient(${angle}deg, oklch(${light} 0 0), oklch(0.1 0 0))`,
			}}
		>
			<span className="absolute inset-0 flex items-center justify-center text-[2.2em] font-light tracking-tight text-white/90">
				{initials(name)}
			</span>
			{showTitleId && (
				<span className="relative m-[0.6em] rounded-full bg-black/40 px-[0.6em] py-[0.15em] font-mono text-[0.55em] tracking-wider text-white/70 backdrop-blur">
					{titleId}
				</span>
			)}
		</div>
	);
}
