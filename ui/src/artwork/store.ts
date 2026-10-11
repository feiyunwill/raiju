import type { Artwork } from "./parse";

const storageKey = "raiju.artwork.v1";
const enabledKey = "raiju.artwork.enabled";
const maxAgeMs = 7 * 24 * 60 * 60 * 1000;

type Entry = { at: number; artwork: Artwork | null };

const memory = new Map<string, Entry>();
const pending = new Map<string, Promise<void>>();
const listeners = new Set<() => void>();
let loaded = false;

function loadPersisted(): void {
	if (loaded) {
		return;
	}
	loaded = true;
	try {
		const raw = localStorage.getItem(storageKey);
		const parsed = raw ? (JSON.parse(raw) as Record<string, Entry>) : {};
		for (const [titleId, entry] of Object.entries(parsed)) {
			if (Date.now() - entry.at < maxAgeMs) {
				memory.set(titleId, entry);
			}
		}
	} catch {
		return;
	}
}

function persist(): void {
	try {
		localStorage.setItem(
			storageKey,
			JSON.stringify(Object.fromEntries(memory)),
		);
	} catch {
		return;
	}
}

function emit(): void {
	for (const listener of listeners) {
		listener();
	}
}

/** Whether cover art lookups are enabled; on unless the viewer turned them off. */
export function artworkEnabled(): boolean {
	try {
		return localStorage.getItem(enabledKey) !== "off";
	} catch {
		return true;
	}
}

/** Turns cover art lookups on or off for this browser. */
export function setArtworkEnabled(enabled: boolean): void {
	try {
		localStorage.setItem(enabledKey, enabled ? "on" : "off");
	} catch {
		return;
	}
	emit();
}

/** Subscribes to artwork cache changes. */
export function subscribeArtwork(listener: () => void): () => void {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

/** Cached artwork for a title, starting a lookup through `/api/art` the first time it is asked for. */
export function getArtwork(titleId: string): Artwork | null {
	loadPersisted();
	if (!artworkEnabled() || !/^(CUSA|PPSA)\d{5}$/.test(titleId)) {
		return null;
	}
	const entry = memory.get(titleId);
	if (entry) {
		return entry.artwork;
	}
	if (!pending.has(titleId)) {
		const request = fetch(`/api/art/${titleId}`)
			.then((response) =>
				response.ok ? (response.json() as Promise<Artwork>) : null,
			)
			.catch(() => null)
			.then((artwork) => {
				memory.set(titleId, { at: Date.now(), artwork });
				pending.delete(titleId);
				persist();
				emit();
			});
		pending.set(titleId, request);
	}
	return null;
}
