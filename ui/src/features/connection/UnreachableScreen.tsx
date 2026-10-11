type UnreachableScreenProps = {
	host: string;
	notice: string | null;
};

/** Full-screen state while the console can't be reached; the source keeps retrying in the background. */
export function UnreachableScreen({ host, notice }: UnreachableScreenProps) {
	return (
		<main className="mx-auto flex min-h-dvh max-w-md flex-col items-center justify-center gap-6 px-6 text-center">
			<span className="relative flex size-3">
				<span className="bg-foreground/40 absolute inline-flex size-full animate-ping rounded-full" />
				<span className="bg-foreground/60 relative inline-flex size-3 rounded-full" />
			</span>
			<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
				Can't reach your PS5
			</h1>
			<p className="text-muted-foreground max-w-sm text-[15px]">
				Make sure it's on and Raiju is running. Retrying{" "}
				<span className="text-foreground font-mono">{host}</span> in the
				background.
			</p>
			{notice && (
				<p className="text-muted-foreground font-mono text-[13px]">{notice}</p>
			)}
		</main>
	);
}
