import type * as React from "react";
import type { Dashboard, DashboardActions } from "@/data/types";
import { PairingScreen } from "./PairingScreen";
import { UnreachableScreen } from "./UnreachableScreen";

/** Renders the connection screens, or the app itself once the console is reachable and paired. */
export function ConnectionGate({
	dashboard,
	actions,
	children,
}: {
	dashboard: Dashboard;
	actions: DashboardActions;
	children: React.ReactNode;
}) {
	if (dashboard.connection === "unpaired") {
		return <PairingScreen notice={dashboard.notice} onPair={actions.pair} />;
	}
	if (dashboard.connection === "unreachable") {
		return (
			<UnreachableScreen
				host={dashboard.console.host}
				notice={dashboard.notice}
			/>
		);
	}
	if (dashboard.connection === "connecting") {
		return (
			<main className="mx-auto flex min-h-dvh max-w-md flex-col items-center justify-center gap-3 px-6 text-center">
				<span className="bg-foreground/50 size-3 animate-pulse rounded-full" />
				<p className="text-muted-foreground text-[15px]">
					Looking for your PS5…
				</p>
			</main>
		);
	}
	return <>{children}</>;
}
