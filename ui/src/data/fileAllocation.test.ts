import { describe, expect, test } from "bun:test";

import {
	fileAllocationFromOption,
	fileAllocationModes,
} from "./fileAllocation";

describe("file allocation modes", () => {
	test("offers only the modes /data can implement", () => {
		expect(fileAllocationModes.map((mode) => mode.id)).toEqual([
			"none",
			"trunc",
		]);
	});

	test("keeps the modes the console implements", () => {
		expect(fileAllocationFromOption("none")).toBe("none");
		expect(fileAllocationFromOption("trunc")).toBe("trunc");
	});

	test("falls back to none for the modes the console refuses", () => {
		for (const value of ["falloc", "prealloc", "", undefined, "garbage"]) {
			expect(fileAllocationFromOption(value)).toBe("none");
		}
	});
});
