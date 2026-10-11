export type Artwork = {
	titleId: string;
	name: string | null;
	icon: string;
	background: string | null;
	logo: string | null;
	source: "orbispatches" | "prosperopatches";
};

const titleIdPattern = /^(CUSA|PPSA)\d{5}$/;

/** Title page that lists artwork for a title ID: OrbisPatches for PS4 (CUSA), ProsperoPatches for PS5 (PPSA). */
export function artworkPageUrl(titleId: string): string | null {
	if (!titleIdPattern.test(titleId)) {
		return null;
	}
	return titleId.startsWith("CUSA")
		? `https://orbispatches.com/${titleId}`
		: `https://prosperopatches.com/${titleId}`;
}

function decodeEntities(text: string): string {
	return text
		.replace(/&#(\d+);/g, (_, code: string) =>
			String.fromCodePoint(Number(code)),
		)
		.replace(/&#x([0-9a-f]+);/gi, (_, code: string) =>
			String.fromCodePoint(Number.parseInt(code, 16)),
		)
		.replace(/&quot;/g, '"')
		.replace(/&#39;|&apos;/g, "'")
		.replace(/&lt;/g, "<")
		.replace(/&gt;/g, ">")
		.replace(/&amp;/g, "&");
}

/**
 * Extracts the official name and the CDN artwork URLs for one title from its OrbisPatches or ProsperoPatches
 * page. Only images under that title's own CDN folder are accepted; returns null when the page has no icon.
 */
export function parseArtworkPage(
	titleId: string,
	html: string,
): Artwork | null {
	const pageUrl = artworkPageUrl(titleId);
	if (!pageUrl) {
		return null;
	}
	const source = titleId.startsWith("CUSA")
		? "orbispatches"
		: "prosperopatches";
	const imagePattern = new RegExp(
		`https://cdn\\.${source}\\.com/titles/${titleId}_[0-9a-f]{16,128}/(icon0|pic0|logo)\\.(?:webp|png|jpg)`,
		"g",
	);
	const images = new Map<string, string>();
	for (const match of html.matchAll(imagePattern)) {
		if (!images.has(match[1])) {
			images.set(match[1], match[0]);
		}
	}
	const icon = images.get("icon0");
	if (!icon) {
		return null;
	}
	const rawTitle =
		html.match(/<meta[^>]+property="og:title"[^>]+content="([^"]*)"/i)?.[1] ??
		html.match(/<title>([^<]*)<\/title>/i)?.[1] ??
		"";
	const name = decodeEntities(rawTitle)
		.replace(new RegExp(`^${titleId}\\s*:\\s*`), "")
		.replace(/\s*\|\s*(ORBIS|PROSPERO)Patches\.com\s*$/i, "")
		.trim();
	return {
		titleId,
		name: name || null,
		icon,
		background: images.get("pic0") ?? null,
		logo: images.get("logo") ?? null,
		source,
	};
}
