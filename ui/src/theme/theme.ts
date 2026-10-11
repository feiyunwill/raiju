export type ThemePreference = "dark" | "light" | "system";

export const THEME_STORAGE_KEY = "raiju.theme";

/** Reads the stored preference, falling back to dark when storage is empty or unavailable. */
export function readThemePreference(): ThemePreference {
	try {
		const stored = localStorage.getItem(THEME_STORAGE_KEY);
		if (stored === "dark" || stored === "light" || stored === "system") {
			return stored;
		}
	} catch {
		return "dark";
	}
	return "dark";
}

/** Persists the preference; storage failures leave the in-memory choice in effect. */
export function writeThemePreference(preference: ThemePreference): void {
	try {
		localStorage.setItem(THEME_STORAGE_KEY, preference);
	} catch {
		return;
	}
}

/** Resolves a preference to the concrete theme applied to the document. */
export function resolveTheme(
	preference: ThemePreference,
	systemPrefersDark: boolean,
): "dark" | "light" {
	if (preference === "system") {
		return systemPrefersDark ? "dark" : "light";
	}
	return preference;
}

/** Toggles the `dark` class on the root element to match the resolved theme. */
export function applyTheme(theme: "dark" | "light"): void {
	document.documentElement.classList.toggle("dark", theme === "dark");
}
