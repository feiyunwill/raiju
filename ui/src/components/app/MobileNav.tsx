import { cn } from "cn";
import { navItems, type View } from "./navigation";

type MobileNavProps = {
	view: View;
	onNavigate: (view: View) => void;
};

/** Floating glass pill navigation for phones. */
export function MobileNav({ view, onNavigate }: MobileNavProps) {
	return (
		<nav
			aria-label="Main"
			className="fixed inset-x-0 bottom-[max(1rem,env(safe-area-inset-bottom))] z-40 flex justify-center px-4 md:hidden"
		>
			<div className="bg-popover/75 flex gap-1 rounded-full p-1.5 shadow-2xl ring-1 ring-foreground/10 backdrop-blur-2xl">
				{navItems.map(({ id, label, icon: Icon }) => (
					<button
						key={id}
						type="button"
						onClick={() => onNavigate(id)}
						aria-current={view === id ? "page" : undefined}
						className={cn(
							"flex h-12 min-w-20 flex-col items-center justify-center gap-0.5 rounded-full px-4 text-[10px] transition-colors duration-150",
							"focus-visible:ring-ring outline-none focus-visible:ring-2",
							view === id
								? "bg-foreground/[0.1] text-foreground"
								: "text-muted-foreground",
						)}
					>
						<Icon className="size-5" strokeWidth={1.5} />
						{label}
					</button>
				))}
			</div>
		</nav>
	);
}
