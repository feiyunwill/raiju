import { Monitor, Moon, Sun } from "lucide-react";
import { ToggleGroup, ToggleGroupItem } from "@/components/ui/toggle-group";
import { useTheme } from "./ThemeProvider";
import type { ThemePreference } from "./theme";

const options: { value: ThemePreference; label: string; icon: typeof Moon }[] =
	[
		{ value: "dark", label: "Dark", icon: Moon },
		{ value: "light", label: "Light", icon: Sun },
		{ value: "system", label: "System", icon: Monitor },
	];

/** Pill toggle for Dark, Light and System appearance. */
export function ThemeToggle() {
	const { preference, setPreference } = useTheme();
	return (
		<ToggleGroup
			type="single"
			size="sm"
			value={preference}
			onValueChange={(value) =>
				value && setPreference(value as ThemePreference)
			}
			aria-label="Appearance"
		>
			{options.map(({ value, label, icon: Icon }) => (
				<ToggleGroupItem key={value} value={value} aria-label={label}>
					<Icon strokeWidth={1.5} />
					<span className="hidden sm:inline">{label}</span>
				</ToggleGroupItem>
			))}
		</ToggleGroup>
	);
}
