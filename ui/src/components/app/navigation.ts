import { Download, House, Settings } from "lucide-react";

export type View = "home" | "downloads" | "settings";

export const navItems: { id: View; label: string; icon: typeof House }[] = [
	{ id: "home", label: "Home", icon: House },
	{ id: "downloads", label: "Downloads", icon: Download },
	{ id: "settings", label: "Settings", icon: Settings },
];
