import { Link2 } from "lucide-react";
import { useState } from "react";
import { Button } from "@/components/ui/button";
import {
	Dialog,
	DialogContent,
	DialogDescription,
	DialogFooter,
	DialogHeader,
	DialogTitle,
} from "@/components/ui/dialog";
import { Textarea } from "@/components/ui/textarea";
import { jobFromLink } from "@/data/useDashboard";

type AddDownloadDialogProps = {
	open: boolean;
	onOpenChange: (open: boolean) => void;
	onAdd: (links: string[]) => void;
};

function parseLinks(text: string): string[] {
	return text
		.split(/\s+/)
		.map((part) => part.trim())
		.filter((part) => /^(https?|ftp|magnet):/i.test(part));
}

/** Paste one or more links; each one shows the install name Raiju will use before it is queued. */
export function AddDownloadDialog({
	open,
	onOpenChange,
	onAdd,
}: AddDownloadDialogProps) {
	const [text, setText] = useState("");
	const links = parseLinks(text);
	const submit = () => {
		onAdd(links);
		setText("");
		onOpenChange(false);
	};
	return (
		<Dialog open={open} onOpenChange={onOpenChange}>
			<DialogContent className="gap-6 rounded-3xl p-6 sm:max-w-lg">
				<DialogHeader>
					<DialogTitle className="text-xl font-normal">
						Add download
					</DialogTitle>
					<DialogDescription>
						Links go straight to your PS5. Archives are extracted and installed
						for you.
					</DialogDescription>
				</DialogHeader>
				<Textarea
					autoFocus
					value={text}
					onChange={(event) => setText(event.target.value)}
					onKeyDown={(event) => {
						if (
							event.key === "Enter" &&
							(event.metaKey || event.ctrlKey) &&
							links.length > 0
						) {
							submit();
						}
					}}
					placeholder="https://example.com/PPSA24003-some-game.7z"
					className="min-h-28 rounded-2xl font-mono text-[13px]"
				/>
				{links.length > 0 && (
					<ul className="flex max-h-40 flex-col gap-2 overflow-y-auto">
						{links.map((link, i) => {
							const job = jobFromLink(link, i);
							return (
								<li
									key={link}
									className="bg-foreground/[0.04] flex items-center gap-3 rounded-2xl px-3 py-2.5 text-[13px]"
								>
									<Link2
										className="text-muted-foreground size-4 shrink-0"
										strokeWidth={1.5}
									/>
									<span className="min-w-0 flex-1 truncate">
										{job.sourceName}
									</span>
									<span className="text-muted-foreground shrink-0 font-mono text-xs">
										→ {job.installName}
									</span>
								</li>
							);
						})}
					</ul>
				)}
				<DialogFooter className="gap-2">
					<Button variant="ghost" onClick={() => onOpenChange(false)}>
						Cancel
					</Button>
					<Button disabled={links.length === 0} onClick={submit}>
						{links.length > 1
							? `Add ${links.length} downloads`
							: "Add download"}
					</Button>
				</DialogFooter>
			</DialogContent>
		</Dialog>
	);
}
