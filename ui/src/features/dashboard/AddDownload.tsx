import { cn } from "cn";
import { FileUp, Link2, X } from "lucide-react";
import { type DragEvent, useEffect, useId, useState } from "react";
import { Button } from "@/components/ui/button";
import {
	Dialog,
	DialogContent,
	DialogDescription,
	DialogHeader,
	DialogTitle,
} from "@/components/ui/dialog";
import {
	Drawer,
	DrawerContent,
	DrawerDescription,
	DrawerHeader,
	DrawerTitle,
} from "@/components/ui/drawer";
import { Input } from "@/components/ui/input";
import { Switch } from "@/components/ui/switch";
import { Textarea } from "@/components/ui/textarea";
import { describeSource } from "@/data/describe";
import {
	type AddOptions,
	destinations,
	type FolderRoot,
	type ProbeVerdict,
} from "@/data/types";
import { useMediaQuery } from "@/hooks/useMediaQuery";

type AddDownloadProps = {
	open: boolean;
	onOpenChange: (open: boolean) => void;
	onAddLinks: (links: string[], options: AddOptions) => void;
	onAddTorrents: (files: File[], options: AddOptions) => void;
	roots: FolderRoot[];
	onProbe: (uri: string) => Promise<ProbeVerdict | null>;
};

const title = "Add download";
const description =
	"Links go straight to your PS5. Archives are extracted and installed for you.";

const probeDelayMs = 400;

function parseLinks(text: string): string[] {
	return [
		...new Set(
			text
				.split(/\s+/)
				.map((part) => part.trim())
				.filter((part) => /^(https?|ftp|magnet):/i.test(part)),
		),
	];
}

function linkLabel(link: string): string {
	if (link.toLowerCase().startsWith("magnet:")) {
		return (
			new URLSearchParams(link.slice(link.indexOf("?") + 1)).get("dn") ??
			"Magnet link"
		);
	}
	return describeSource(link.split(/[?#]/)[0]?.split("/").pop() || link)
		.sourceName;
}

function isProbeable(link: string): boolean {
	return /^https?:/i.test(link);
}

function AddDownloadForm({
	onAddLinks,
	onAddTorrents,
	roots,
	onProbe,
	onDone,
}: Omit<AddDownloadProps, "open" | "onOpenChange"> & { onDone: () => void }) {
	const pills: FolderRoot[] =
		roots.length > 0 ? roots : destinations.map((root) => ({ ...root }));
	const [text, setText] = useState("");
	const [torrents, setTorrents] = useState<File[]>([]);
	const [destination, setDestination] = useState<string>(pills[0].path);
	const [titleOverride, setTitleOverride] = useState<{
		link: string;
		value: string;
	} | null>(null);
	const [keepArchive, setKeepArchive] = useState(false);
	const [verdict, setVerdict] = useState<ProbeVerdict | null>(null);
	const [probing, setProbing] = useState(false);
	const [dragging, setDragging] = useState(false);
	const inputId = useId();
	const links = parseLinks(text);
	const total = links.length + torrents.length;
	const probeLink = links.find(isProbeable);
	const detected = describeSource(probeLink ? linkLabel(probeLink) : "");
	const detectedTitleId =
		detected.titleId === "UNKNOWN" ? null : detected.titleId;
	const shownTitleId =
		titleOverride && titleOverride.link === probeLink
			? titleOverride.value
			: (detectedTitleId ?? "");
	const extension = detected.kind === "image" ? detected.format : "exfat";
	const installName = shownTitleId ? `${shownTitleId}.${extension}` : null;

	useEffect(() => {
		if (!probeLink) {
			setVerdict(null);
			setProbing(false);
			return;
		}
		let cancelled = false;
		setVerdict(null);
		setProbing(true);
		const timer = setTimeout(() => {
			onProbe(probeLink)
				.then((result) => {
					if (!cancelled) {
						setVerdict(result);
						setProbing(false);
					}
				})
				.catch(() => {
					if (!cancelled) {
						setVerdict(null);
						setProbing(false);
					}
				});
		}, probeDelayMs);
		return () => {
			cancelled = true;
			clearTimeout(timer);
		};
	}, [probeLink, onProbe]);

	const addFiles = (files: FileList | null) => {
		const picked = Array.from(files ?? []).filter((file) =>
			file.name.toLowerCase().endsWith(".torrent"),
		);
		setTorrents((current) => [
			...current,
			...picked.filter((file) => !current.some((c) => c.name === file.name)),
		]);
	};

	const onDrop = (event: DragEvent) => {
		event.preventDefault();
		setDragging(false);
		addFiles(event.dataTransfer.files);
	};

	const submit = () => {
		const options: AddOptions = {
			destination,
			titleId: links.length === 1 && shownTitleId ? shownTitleId : undefined,
			keepArchive,
		};
		if (links.length > 0) {
			onAddLinks(links, options);
		}
		if (torrents.length > 0) {
			onAddTorrents(torrents, options);
		}
		setText("");
		setTorrents([]);
		onDone();
	};

	return (
		<div className="flex flex-col gap-5">
			<Textarea
				autoFocus
				value={text}
				onChange={(event) => setText(event.target.value)}
				onKeyDown={(event) => {
					if (
						event.key === "Enter" &&
						(event.metaKey || event.ctrlKey) &&
						total > 0
					) {
						submit();
					}
				}}
				placeholder="https://example.com/PPSA24003-some-game.7z"
				aria-label="Links, one per line"
				className="min-h-28 rounded-2xl font-mono text-[13px]"
			/>
			<label
				htmlFor={inputId}
				onDragOver={(event) => {
					event.preventDefault();
					setDragging(true);
				}}
				onDragLeave={() => setDragging(false)}
				onDrop={onDrop}
				className={cn(
					"text-muted-foreground flex cursor-pointer items-center justify-center gap-2 rounded-2xl border border-dashed px-4 py-4 text-[13px] transition-colors",
					dragging
						? "border-foreground bg-foreground/[0.06] text-foreground"
						: "border-foreground/15 hover:border-foreground/30",
				)}
			>
				<FileUp className="size-4" strokeWidth={1.5} />
				Drop .torrent files or browse
				<input
					id={inputId}
					type="file"
					accept=".torrent,application/x-bittorrent"
					multiple
					className="sr-only"
					onChange={(event) => addFiles(event.target.files)}
				/>
			</label>
			{probeLink && (
				<div
					aria-live="polite"
					className="bg-foreground/[0.04] flex flex-wrap items-center gap-x-3 gap-y-1 rounded-2xl px-3 py-2.5 font-mono text-[11px]"
				>
					{verdict ? (
						<>
							<span>{verdict.badge}</span>
							<span className="text-muted-foreground">{verdict.size}</span>
							<span>{verdict.extracted}</span>
							<span className="text-muted-foreground">{verdict.required}</span>
							<span className={verdict.destructive ? "text-destructive" : ""}>
								{verdict.verdict}
							</span>
						</>
					) : (
						<span className="text-muted-foreground">
							{probing
								? "Checking the link…"
								: "Raiju couldn't size this link up."}
						</span>
					)}
				</div>
			)}
			{installName && (
				<fieldset className="flex flex-col gap-2">
					<legend className="text-muted-foreground mb-2 text-xs">
						Will install as
					</legend>
					<div className="flex flex-wrap items-center gap-2 font-mono text-[13px]">
						<Input
							value={shownTitleId}
							onChange={(event) =>
								setTitleOverride({
									link: probeLink ?? "",
									value: event.target.value
										.toUpperCase()
										.replace(/[^A-Z0-9]/g, "")
										.slice(0, 9),
								})
							}
							aria-label="Title ID"
							className="w-32 rounded-full text-center"
						/>
						<span>→ {installName}</span>
					</div>
				</fieldset>
			)}
			{total > 0 && (
				<ul className="flex max-h-44 flex-col gap-2 overflow-y-auto">
					{links.map((link) => {
						const label = linkLabel(link);
						return (
							<li
								key={link}
								className="bg-foreground/[0.04] flex items-center gap-3 rounded-2xl px-3 py-2.5 text-[13px]"
							>
								<Link2
									className="text-muted-foreground size-4 shrink-0"
									strokeWidth={1.5}
								/>
								<span className="min-w-0 flex-1 truncate">{label}</span>
								<span className="text-muted-foreground shrink-0 font-mono text-xs">
									→ {describeSource(label).installName}
								</span>
							</li>
						);
					})}
					{torrents.map((file) => (
						<li
							key={file.name}
							className="bg-foreground/[0.04] flex items-center gap-3 rounded-2xl px-3 py-2.5 text-[13px]"
						>
							<FileUp
								className="text-muted-foreground size-4 shrink-0"
								strokeWidth={1.5}
							/>
							<span className="min-w-0 flex-1 truncate">{file.name}</span>
							<button
								type="button"
								aria-label={`Remove ${file.name}`}
								onClick={() =>
									setTorrents((current) =>
										current.filter((item) => item !== file),
									)
								}
								className="text-muted-foreground hover:text-foreground focus-visible:ring-ring rounded-full p-1 outline-none focus-visible:ring-2"
							>
								<X className="size-3.5" />
							</button>
						</li>
					))}
				</ul>
			)}
			<fieldset className="flex flex-col gap-2">
				<legend className="text-muted-foreground mb-2 text-xs">
					Install to
				</legend>
				<div className="flex flex-wrap gap-2">
					{pills.map((item) => (
						<label
							key={item.id}
							className={cn(
								"flex h-9 cursor-pointer items-center rounded-full px-4 font-mono text-xs transition-colors duration-150",
								"has-[:focus-visible]:ring-ring has-[:focus-visible]:ring-offset-background has-[:focus-visible]:ring-2 has-[:focus-visible]:ring-offset-2",
								destination === item.path
									? "bg-primary text-primary-foreground"
									: "bg-foreground/[0.05] text-muted-foreground hover:text-foreground",
							)}
						>
							<input
								type="radio"
								name={`${inputId}-destination`}
								value={item.path}
								checked={destination === item.path}
								onChange={() => setDestination(item.path)}
								className="sr-only"
							/>
							{item.label}
						</label>
					))}
				</div>
			</fieldset>
			<fieldset className="flex flex-col gap-2">
				<legend className="text-muted-foreground mb-2 text-xs">
					After download
				</legend>
				<div className="flex items-center justify-between gap-4">
					<span className="text-[13px]">Delete archive after extracting</span>
					<Switch
						checked={!keepArchive}
						onCheckedChange={(on) => setKeepArchive(!on)}
						aria-label="Delete archive after extracting"
					/>
				</div>
			</fieldset>
			<div className="flex flex-col-reverse gap-2 sm:flex-row sm:justify-end">
				<Button variant="ghost" onClick={onDone}>
					Cancel
				</Button>
				<Button disabled={total === 0} onClick={submit}>
					{total > 1 ? `Add ${total} downloads` : title}
				</Button>
			</div>
		</div>
	);
}

/** Add-download flow: a dialog on desktop and a bottom drawer on phones. */
export function AddDownload(props: AddDownloadProps) {
	const desktop = useMediaQuery("(min-width: 768px)");
	const close = () => props.onOpenChange(false);
	if (desktop) {
		return (
			<Dialog open={props.open} onOpenChange={props.onOpenChange}>
				<DialogContent className="gap-6 rounded-3xl p-6 sm:max-w-lg">
					<DialogHeader>
						<DialogTitle className="text-xl font-normal">{title}</DialogTitle>
						<DialogDescription>{description}</DialogDescription>
					</DialogHeader>
					<AddDownloadForm {...props} onDone={close} />
				</DialogContent>
			</Dialog>
		);
	}
	return (
		<Drawer open={props.open} onOpenChange={props.onOpenChange}>
			<DrawerContent className="rounded-t-3xl px-4 pb-[max(1.5rem,env(safe-area-inset-bottom))]">
				<DrawerHeader className="px-0 text-left">
					<DrawerTitle className="text-xl font-normal">{title}</DrawerTitle>
					<DrawerDescription>{description}</DrawerDescription>
				</DrawerHeader>
				<AddDownloadForm {...props} onDone={close} />
			</DrawerContent>
		</Drawer>
	);
}
