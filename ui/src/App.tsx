import { X } from "lucide-react";
import { useEffect, useState } from "react";
import { AppHeader } from "@/components/app/AppHeader";
import { MobileNav } from "@/components/app/MobileNav";
import type { View } from "@/components/app/navigation";
import { useDashboard } from "@/data/useDashboard";
import { ConnectionGate } from "@/features/connection/ConnectionGate";
import { AddDownload } from "@/features/dashboard/AddDownload";
import { DashboardView } from "@/features/dashboard/DashboardView";
import { DownloadsView } from "@/features/dashboard/DownloadsView";
import { JobDetailSheet } from "@/features/dashboard/JobDetailSheet";
import { SettingsView } from "@/features/settings/SettingsView";

/** Application shell: connection gate, header, current view, mobile navigation, add flow and detail sheet. */
export function App() {
	const [view, setView] = useState<View>("home");
	const [adding, setAdding] = useState(false);
	const [detailId, setDetailId] = useState<string | null>(null);
	const [dashboard, actions, mode] = useDashboard();

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
			<ConnectionGate dashboard={dashboard} actions={actions}>
				<AppHeader
					view={view}
					onNavigate={setView}
					console={dashboard.console}
					mode={mode}
					onAdd={() => setAdding(true)}
				/>
				<div className="mx-auto flex max-w-7xl flex-col px-4 md:px-8">
					{dashboard.notice && (
						<div
							role="alert"
							className="bg-destructive/10 text-destructive mt-2 flex items-center justify-between gap-3 rounded-2xl px-4 py-3 text-[13px]"
						>
							<span className="min-w-0 truncate">{dashboard.notice}</span>
							<button
								type="button"
								aria-label="Dismiss"
								onClick={actions.dismissNotice}
								className="focus-visible:ring-ring rounded-full p-1 outline-none focus-visible:ring-2"
							>
								<X className="size-4" />
							</button>
						</div>
					)}
					<main className="flex-1 pt-4 pb-32 md:pt-6 md:pb-16">
						{view === "home" && (
							<DashboardView
								dashboard={dashboard}
								actions={actions}
								onAdd={() => setAdding(true)}
								onViewAll={() => setView("downloads")}
								onOpenDetail={setDetailId}
							/>
						)}
						{view === "downloads" && (
							<DownloadsView
								jobs={dashboard.jobs}
								actions={actions}
								onOpenDetail={setDetailId}
							/>
						)}
						{view === "settings" && (
							<SettingsView
								console={dashboard.console}
								mode={mode}
								folders={dashboard.folders}
								actions={actions}
							/>
						)}
					</main>
				</div>
				<MobileNav view={view} onNavigate={setView} />
				<AddDownload
					open={adding}
					onOpenChange={setAdding}
					onAddLinks={actions.addLinks}
					onAddTorrents={actions.addTorrents}
					roots={dashboard.folders}
					onProbe={actions.probe}
				/>
				<JobDetailSheet
					jobId={detailId}
					actions={actions}
					onClose={() => setDetailId(null)}
				/>
			</ConnectionGate>
		</div>
	);
}
