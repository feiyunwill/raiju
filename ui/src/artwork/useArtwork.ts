import { useSyncExternalStore } from "react";
import type { Artwork } from "./parse";
import {
	artworkEnabled,
	getArtwork,
	setArtworkEnabled,
	subscribeArtwork,
} from "./store";

/** Official name, cover and background for a title ID, or null while unknown or disabled. */
export function useArtwork(titleId: string): Artwork | null {
	return useSyncExternalStore(subscribeArtwork, () => getArtwork(titleId));
}

/** Whether cover art is enabled, and a setter; shared across the app. */
export function useArtworkEnabled(): [boolean, (enabled: boolean) => void] {
	const enabled = useSyncExternalStore(subscribeArtwork, artworkEnabled);
	return [enabled, setArtworkEnabled];
}
