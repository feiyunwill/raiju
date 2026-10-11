import { cn } from "cn";
import { Plus, Zap } from "lucide-react";
import { Button } from "@/components/ui/button";
import type { Console } from "@/data/types";
import { navItems, type View } from "./navigation";

type AppHeaderProps = {
	view: View;
	onNavigate: (view: View) => void;
	console: Console;
	onAdd: () => void;
};

/** Top bar: wordmark, centred pill navigation on desktop, console status and the primary add action. */
export function AppHeader({
	view,
	onNavigate,
	console,
	onAdd,
}: AppHeaderProps) {
	return (
		<header className="sticky top-0 z-30 backdrop-blur-xl [mask-image:linear-gradient(black_80%,transparent)]">
			<div className="mx-auto flex h-16 max-w-7xl items-center gap-4 px-4 md:h-20 md:px-8">
				<div className="flex flex-1 items-center gap-2.5">
					<span className="bg-foreground text-background flex size-8 items-center justify-center rounded-xl">
						<Zap className="size-4" strokeWidth={2} fill="currentColor" />
					</span>
					<span className="text-[15px] font-medium tracking-[0.32em]">
						RAIJU
					</span>
				</div>
				<nav
					aria-label="Main"
					className="bg-foreground/[0.04] hidden items-center gap-1 rounded-full p-1 ring-1 ring-foreground/[0.06] md:flex"
				>
					{navItems.map((item) => (
						<button
							key={item.id}
							type="button"
							onClick={() => onNavigate(item.id)}
							aria-current={view === item.id ? "page" : undefined}
							className={cn(
								"h-9 rounded-full px-5 text-[13px] transition-colors duration-150",
								"focus-visible:ring-ring focus-visible:ring-offset-background outline-none focus-visible:ring-2 focus-visible:ring-offset-2",
								view === item.id
									? "bg-foreground text-background"
									: "text-muted-foreground hover:text-foreground",
							)}
						>
							{item.label}
						</button>
					))}
				</nav>
				<div className="flex flex-1 items-center justify-end gap-3">
					<div className="text-muted-foreground hidden items-center gap-2 rounded-full px-3 py-1.5 text-xs ring-1 ring-foreground/[0.08] sm:flex">
						<span className="relative flex size-2">
							{console.online && (
								<span className="bg-foreground absolute inline-flex size-full animate-ping rounded-full opacity-50" />
							)}
							<span
								className={cn(
									"relative inline-flex size-2 rounded-full",
									console.online ? "bg-foreground" : "bg-destructive",
								)}
							/>
						</span>
						<span>PS5</span>
						<span className="text-foreground font-mono">{console.host}</span>
					</div>
					<Button onClick={onAdd} className="hidden md:inline-flex">
						<Plus strokeWidth={1.5} />
						Add download
						<kbd className="bg-primary-foreground/10 ml-1 rounded-md px-1.5 font-sans text-[11px]">
							⌘K
						</kbd>
					</Button>
					<Button
						onClick={onAdd}
						size="icon"
						aria-label="Add download"
						className="md:hidden"
					>
						<Plus strokeWidth={1.5} />
					</Button>
				</div>
			</div>
		</header>
	);
}
