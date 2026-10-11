import { useCallback, useEffect, useState } from "react";
import { createMockDashboard, tickMockDashboard } from "./mock";
import type { Dashboard, Job, JobKind, JobStatus } from "./types";

export type DashboardActions = {
	togglePause: (id: string) => void;
	pauseAll: () => void;
	resumeAll: () => void;
	retry: (id: string) => void;
	addLinks: (links: string[]) => void;
};

function setStatus(
	state: Dashboard,
	predicate: (status: JobStatus) => boolean,
	status: JobStatus,
): Dashboard {
	return {
		...state,
		jobs: state.jobs.map((job) =>
			predicate(job.status)
				? { ...job, status, speed: 0, connections: 0 }
				: job,
		),
	};
}

const imageFormats = ["ffpkg", "exfat", "ffpfs", "ffpfsc"];

/** Builds a waiting demo job from a pasted link, guessing title ID, kind and install name from the file name. */
export function jobFromLink(link: string, index: number): Job {
	const sourceName = decodeURIComponent(
		link.split(/[?#]/)[0].split("/").pop() || link,
	);
	const format = (
		sourceName.match(/\.([a-z0-9]+)$/i)?.[1] ?? "bin"
	).toLowerCase();
	const titleId = sourceName.match(/(PPSA|CUSA)\d{5}/)?.[0] ?? "UNKNOWN";
	const kind: JobKind = imageFormats.includes(format) ? "image" : "archive";
	const name = sourceName
		.replace(/\.[^.]+$/, "")
		.replace(/(PPSA|CUSA)\d{5}/, "")
		.replace(/[-_.]+/g, " ")
		.trim()
		.replace(/\b\w/g, (c) => c.toUpperCase());
	const totalBytes = (18 + ((index * 7919) % 50)) * 1_000_000_000;
	return {
		id: `${Date.now().toString(36)}-${index}`,
		titleId,
		name: name || titleId,
		sourceName,
		kind,
		format,
		installName: kind === "image" ? `${titleId}.${format}` : `${titleId}.exfat`,
		stage: "download",
		status: "waiting",
		totalBytes,
		stageBytes: totalBytes,
		stageDoneBytes: 0,
		speed: 0,
		connections: 0,
	};
}

/** Live dashboard state. Backed by a one-second demo simulation until the aria2 client (#10) replaces it. */
export function useDashboard(): [Dashboard, DashboardActions] {
	const [state, setState] = useState(createMockDashboard);

	useEffect(() => {
		const timer = window.setInterval(() => {
			if (document.visibilityState === "visible") {
				setState(tickMockDashboard);
			}
		}, 1000);
		return () => window.clearInterval(timer);
	}, []);

	const togglePause = useCallback((id: string) => {
		setState((current) => ({
			...current,
			jobs: current.jobs.map((job) => {
				if (job.id !== id) {
					return job;
				}
				if (job.status === "active") {
					return { ...job, status: "paused", speed: 0, connections: 0 };
				}
				if (job.status === "paused" || job.status === "waiting") {
					return { ...job, status: "active" };
				}
				return job;
			}),
		}));
	}, []);

	const pauseAll = useCallback(
		() =>
			setState((current) =>
				setStatus(current, (s) => s === "active" || s === "waiting", "paused"),
			),
		[],
	);

	const resumeAll = useCallback(
		() =>
			setState((current) =>
				setStatus(current, (s) => s === "paused", "active"),
			),
		[],
	);

	const retry = useCallback((id: string) => {
		setState((current) => ({
			...current,
			jobs: current.jobs.map((job) =>
				job.id === id
					? {
							...job,
							status: "waiting",
							stage: "download",
							stageDoneBytes: 0,
							error: undefined,
						}
					: job,
			),
		}));
	}, []);

	const addLinks = useCallback((links: string[]) => {
		setState((current) => ({
			...current,
			jobs: [...current.jobs, ...links.map(jobFromLink)],
		}));
	}, []);

	return [state, { togglePause, pauseAll, resumeAll, retry, addLinks }];
}
