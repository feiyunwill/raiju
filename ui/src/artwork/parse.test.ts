import { describe, expect, test } from "bun:test";
import { artworkPageUrl, parseArtworkPage } from "./parse";

const ps5Page = `<html><head><title>PPSA25380: ELDEN RING NIGHTREIGN</title>
<meta property="og:title" content="PPSA25380: ELDEN RING NIGHTREIGN">
<meta name="twitter:image" content="https://cdn.prosperopatches.com/titles/PPSA25380_0fc378c528c70c9e1df85472c51743f708b151221b97c3d2d87ee8d9eb01651b/icon0.webp">
</head><body><img src="https://cdn.prosperopatches.com/titles/PPSA25380_0fc378c528c70c9e1df85472c51743f708b151221b97c3d2d87ee8d9eb01651b/pic0.webp">
<img src="https://cdn.prosperopatches.com/titles/PPSA99999_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/icon0.webp"></body></html>`;

const ps4Page = `<title>CUSA00003: DRIVECLUB&#8482; | ORBISPatches.com</title>
<img src="https://cdn.orbispatches.com/titles/CUSA00003_ee91ab19c76a44bec5b591462cf4576abc77fcd658c101851f10d91a63962bca/icon0.webp">
<img src="https://cdn.orbispatches.com/titles/CUSA00003_ee91ab19c76a44bec5b591462cf4576abc77fcd658c101851f10d91a63962bca/logo.webp">`;

describe("artworkPageUrl", () => {
	test("routes PS4 titles to OrbisPatches and PS5 titles to ProsperoPatches", () => {
		expect(artworkPageUrl("CUSA00003")).toBe(
			"https://orbispatches.com/CUSA00003",
		);
		expect(artworkPageUrl("PPSA25380")).toBe(
			"https://prosperopatches.com/PPSA25380",
		);
	});

	test("rejects anything that is not a title ID", () => {
		expect(artworkPageUrl("UNKNOWN")).toBeNull();
		expect(artworkPageUrl("PPSA2538")).toBeNull();
		expect(artworkPageUrl("PPSA25380/../admin")).toBeNull();
	});
});

describe("parseArtworkPage", () => {
	test("extracts name, icon and background from a ProsperoPatches page", () => {
		const art = parseArtworkPage("PPSA25380", ps5Page);
		expect(art?.name).toBe("ELDEN RING NIGHTREIGN");
		expect(art?.icon).toEndWith(
			"PPSA25380_0fc378c528c70c9e1df85472c51743f708b151221b97c3d2d87ee8d9eb01651b/icon0.webp",
		);
		expect(art?.background).toEndWith("/pic0.webp");
		expect(art?.logo).toBeNull();
		expect(art?.source).toBe("prosperopatches");
	});

	test("decodes entities and strips the site suffix on OrbisPatches pages", () => {
		const art = parseArtworkPage("CUSA00003", ps4Page);
		expect(art?.name).toBe("DRIVECLUB™");
		expect(art?.logo).toEndWith("/logo.webp");
		expect(art?.background).toBeNull();
	});

	test("ignores images belonging to other titles or hosts", () => {
		expect(parseArtworkPage("PPSA99998", ps5Page)).toBeNull();
		const spoofed = `<img src="https://evil.example/titles/PPSA25380_${"a".repeat(64)}/icon0.webp">`;
		expect(parseArtworkPage("PPSA25380", spoofed)).toBeNull();
	});

	test("returns null when the page has no icon, as for unknown title IDs", () => {
		expect(
			parseArtworkPage("PPSA99999", "<title>PPSA99999</title>"),
		).toBeNull();
	});
});
