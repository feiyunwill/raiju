import {
	createContext,
	type ReactNode,
	useContext,
	useEffect,
	useMemo,
	useState,
} from "react";
import {
	applyTheme,
	readThemePreference,
	resolveTheme,
	type ThemePreference,
	writeThemePreference,
} from "./theme";

type ThemeContextValue = {
	preference: ThemePreference;
	theme: "dark" | "light";
	setPreference: (preference: ThemePreference) => void;
};

const ThemeContext = createContext<ThemeContextValue | null>(null);

const darkQuery = "(prefers-color-scheme: dark)";

/** Owns the theme preference, keeps the document class in sync, and follows the OS when set to system. */
export function ThemeProvider({ children }: { children: ReactNode }) {
	const [preference, setPreferenceState] =
		useState<ThemePreference>(readThemePreference);
	const [systemPrefersDark, setSystemPrefersDark] = useState(
		() => window.matchMedia(darkQuery).matches,
	);

	useEffect(() => {
		const media = window.matchMedia(darkQuery);
		const onChange = (event: MediaQueryListEvent) =>
			setSystemPrefersDark(event.matches);
		media.addEventListener("change", onChange);
		return () => media.removeEventListener("change", onChange);
	}, []);

	const theme = resolveTheme(preference, systemPrefersDark);

	useEffect(() => {
		applyTheme(theme);
	}, [theme]);

	const value = useMemo<ThemeContextValue>(
		() => ({
			preference,
			theme,
			setPreference: (next) => {
				writeThemePreference(next);
				setPreferenceState(next);
			},
		}),
		[preference, theme],
	);

	return (
		<ThemeContext.Provider value={value}>{children}</ThemeContext.Provider>
	);
}

/** Returns the current theme state; must be used inside ThemeProvider. */
export function useTheme(): ThemeContextValue {
	const value = useContext(ThemeContext);
	if (!value) {
		throw new Error("useTheme must be used inside ThemeProvider");
	}
	return value;
}
