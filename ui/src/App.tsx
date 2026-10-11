import { useEffect, useState } from "react";
import { AppHeader } from "@/components/app/AppHeader";
import { MobileNav } from "@/components/app/MobileNav";
import type { View } from "@/components/app/navigation";
import { useDashboard } from "@/data/useDashboard";
import { AddDownloadDialog } from "@/features/dashboard/AddDownloadDialog";
import { DashboardView } from "@/features/dashboard/DashboardView";
import { DownloadsView } from "@/features/dashboard/DownloadsView";
import { SettingsView } from "@/features/settings/SettingsView";

/** Application shell: header, current view, mobile navigation and the add-download dialog. */
export function App() {
	const [view, setView] = useState<View>("home");
	const [adding, setAdding] = useState(false);
	const [dashboard, actions] = useDashboard();

	useEffect(() => {
		const onKeyDown = (event: KeyboardEvent) => {
			if (event.key.toLowerCase() === "k" && (event.metaKey || event.ctrlKey)) {
				event.preventDefault();
				setAdding(true);
			}
		};
		window.addEventListener("keydown", onKeyDown);
		return () => window.removeEventListener("keydown", onKeyDown);
	}, []);

	return (
		<div className="backdrop-glow min-h-dvh">
			<AppHeader
				view={view}
				onNavigate={setView}
				console={dashboard.console}
				onAdd={() => setAdding(true)}
			/>
			<div className="mx-auto flex max-w-7xl flex-col px-4 md:px-8">
				<main className="flex-1 pt-4 pb-32 md:pt-6 md:pb-16">
					{view === "home" && (
						<DashboardView
							dashboard={dashboard}
							actions={actions}
							onAdd={() => setAdding(true)}
							onViewAll={() => setView("downloads")}
						/>
					)}
					{view === "downloads" && (
						<DownloadsView jobs={dashboard.jobs} actions={actions} />
					)}
					{view === "settings" && <SettingsView console={dashboard.console} />}
				</main>
			</div>
			<MobileNav view={view} onNavigate={setView} />
			<AddDownloadDialog
				open={adding}
				onOpenChange={setAdding}
				onAdd={actions.addLinks}
			/>
		</div>
	);
}
