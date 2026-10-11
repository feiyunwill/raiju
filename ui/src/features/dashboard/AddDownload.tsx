import { cn } from "cn";
import { FileUp, Link2, X } from "lucide-react";
import { type DragEvent, useId, useState } from "react";
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
import { Textarea } from "@/components/ui/textarea";
import { describeSource } from "@/data/describe";
import { type AddOptions, destinations } from "@/data/types";
import { useMediaQuery } from "@/hooks/useMediaQuery";

type AddDownloadProps = {
	open: boolean;
	onOpenChange: (open: boolean) => void;
	onAddLinks: (links: string[], options: AddOptions) => void;
	onAddTorrents: (files: File[], options: AddOptions) => void;
};

const title = "Add download";
const description =
	"Links go straight to your PS5. Archives are extracted and installed for you.";

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

function AddDownloadForm({
	onAddLinks,
	onAddTorrents,
	onDone,
}: Omit<AddDownloadProps, "open" | "onOpenChange"> & { onDone: () => void }) {
	const [text, setText] = useState("");
	const [torrents, setTorrents] = useState<File[]>([]);
	const [destination, setDestination] = useState<string>(destinations[0].path);
	const [dragging, setDragging] = useState(false);
	const inputId = useId();
	const links = parseLinks(text);
	const total = links.length + torrents.length;

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
		const options = { destination };
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
					{destinations.map((item) => (
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
