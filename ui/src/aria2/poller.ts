type VisibilityTarget = Pick<
	Document,
	"visibilityState" | "addEventListener" | "removeEventListener"
>;

export type Poller = {
	stop: () => void;
};

/**
 * Runs `task` every `intervalMs` only while the page is visible: it runs once immediately when the page
 * becomes visible, pauses while hidden, and never overlaps a run that is still in flight.
 */
export function pollWhileVisible(
	task: () => Promise<unknown> | unknown,
	intervalMs = 1000,
	target: VisibilityTarget = document,
): Poller {
	let timer: ReturnType<typeof setInterval> | null = null;
	let running = false;
	let stopped = false;

	const run = async () => {
		if (running || stopped) {
			return;
		}
		running = true;
		try {
			await task();
		} catch {
			return;
		} finally {
			running = false;
		}
	};

	const start = () => {
		if (timer !== null || stopped) {
			return;
		}
		void run();
		timer = setInterval(run, intervalMs);
	};

	const pause = () => {
		if (timer !== null) {
			clearInterval(timer);
			timer = null;
		}
	};

	const onVisibilityChange = () => {
		if (target.visibilityState === "visible") {
			start();
		} else {
			pause();
		}
	};

	target.addEventListener("visibilitychange", onVisibilityChange);
	onVisibilityChange();

	return {
		stop: () => {
			stopped = true;
			pause();
			target.removeEventListener("visibilitychange", onVisibilityChange);
		},
	};
}
