import { type Artwork, artworkPageUrl, parseArtworkPage } from "./parse";

type Middleware = (
	req: { url?: string },
	res: {
		statusCode: number;
		setHeader: (name: string, value: string) => void;
		end: (body?: string) => void;
	},
	next: () => void,
) => void;

const ttlMs = 24 * 60 * 60 * 1000;

/**
 * Connect-style middleware serving `GET /api/art/<TITLE_ID>` by fetching and parsing the title's
 * OrbisPatches or ProsperoPatches page once, then answering from an in-memory cache. Mirrors the
 * endpoint the Raiju gateway exposes on the PS5.
 */
export function createArtworkMiddleware(
	fetchImpl: typeof fetch = fetch,
): Middleware {
	const cache = new Map<
		string,
		{ at: number; value: Promise<Artwork | null> }
	>();
	const lookup = (titleId: string): Promise<Artwork | null> => {
		const hit = cache.get(titleId);
		if (hit && Date.now() - hit.at < ttlMs) {
			return hit.value;
		}
		const url = artworkPageUrl(titleId);
		const value = url
			? fetchImpl(url, {
					headers: {
						"User-Agent": "Raiju (+https://github.com/cherfia/raiju)",
					},
				})
					.then((response) => (response.ok ? response.text() : ""))
					.then((html) => parseArtworkPage(titleId, html))
					.catch(() => null)
			: Promise.resolve(null);
		cache.set(titleId, { at: Date.now(), value });
		return value;
	};
	return (req, res, next) => {
		const match = req.url?.match(/^\/api\/art\/([A-Z0-9]+)\/?$/);
		if (!match) {
			next();
			return;
		}
		void lookup(match[1]).then((artwork) => {
			res.setHeader("Content-Type", "application/json");
			res.setHeader("Cache-Control", "public, max-age=86400");
			res.statusCode = artwork ? 200 : 404;
			res.end(JSON.stringify(artwork ?? { error: "not_found" }));
		});
	};
}
