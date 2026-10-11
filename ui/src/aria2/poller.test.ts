import { describe, expect, test } from "bun:test";
import { pollWhileVisible } from "./poller";

class FakeDocument {
	visibilityState: DocumentVisibilityState = "visible";
	private listeners = new Set<() => void>();

	addEventListener(_type: string, listener: () => void) {
		this.listeners.add(listener);
	}

	removeEventListener(_type: string, listener: () => void) {
		this.listeners.delete(listener);
	}

	setVisibility(state: DocumentVisibilityState) {
		this.visibilityState = state;
		for (const listener of this.listeners) {
			listener();
		}
	}

	get listenerCount() {
		return this.listeners.size;
	}
}

const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

describe("pollWhileVisible", () => {
	test("runs immediately and then on every interval while visible", async () => {
		const doc = new FakeDocument();
		let runs = 0;
		const poller = pollWhileVisible(
			() => runs++,
			20,
			doc as unknown as Document,
		);
		await sleep(70);
		poller.stop();
		expect(runs).toBeGreaterThanOrEqual(3);
	});

	test("does not run while hidden and resumes immediately when visible again", async () => {
		const doc = new FakeDocument();
		doc.visibilityState = "hidden";
		let runs = 0;
		const poller = pollWhileVisible(
			() => runs++,
			20,
			doc as unknown as Document,
		);
		await sleep(60);
		expect(runs).toBe(0);
		doc.setVisibility("visible");
		await sleep(5);
		expect(runs).toBe(1);
		doc.setVisibility("hidden");
		const before = runs;
		await sleep(60);
		expect(runs).toBe(before);
		poller.stop();
	});

	test("never overlaps a run that is still in flight", async () => {
		const doc = new FakeDocument();
		let concurrent = 0;
		let maxConcurrent = 0;
		const poller = pollWhileVisible(
			async () => {
				concurrent++;
				maxConcurrent = Math.max(maxConcurrent, concurrent);
				await sleep(50);
				concurrent--;
			},
			10,
			doc as unknown as Document,
		);
		await sleep(120);
		poller.stop();
		expect(maxConcurrent).toBe(1);
	});

	test("stop removes the visibility listener", () => {
		const doc = new FakeDocument();
		const poller = pollWhileVisible(
			() => undefined,
			20,
			doc as unknown as Document,
		);
		expect(doc.listenerCount).toBe(1);
		poller.stop();
		expect(doc.listenerCount).toBe(0);
	});
});
