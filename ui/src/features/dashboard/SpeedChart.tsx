import { useId } from "react";

type SpeedChartProps = {
	samples: number[];
	className?: string;
};

const width = 600;
const height = 160;

function toPoints(samples: number[], max: number): [number, number][] {
	const step = width / Math.max(1, samples.length - 1);
	return samples.map((value, i) => [
		i * step,
		height - (value / max) * (height - 12),
	]);
}

function smoothPath(points: [number, number][]): string {
	if (points.length === 0) {
		return "";
	}
	let path = `M${points[0][0]},${points[0][1]}`;
	for (let i = 0; i < points.length - 1; i++) {
		const [x0, y0] = points[Math.max(0, i - 1)];
		const [x1, y1] = points[i];
		const [x2, y2] = points[i + 1];
		const [x3, y3] = points[Math.min(points.length - 1, i + 2)];
		const c1x = x1 + (x2 - x0) / 6;
		const c1y = y1 + (y2 - y0) / 6;
		const c2x = x2 - (x3 - x1) / 6;
		const c2y = y2 - (y3 - y1) / 6;
		path += ` C${c1x},${c1y} ${c2x},${c2y} ${x2},${y2}`;
	}
	return path;
}

/** Smooth area chart of the last 60 speed samples, drawn in the current text colour. */
export function SpeedChart({ samples, className }: SpeedChartProps) {
	const gradientId = useId();
	const max = Math.max(1, ...samples) * 1.15;
	const line = smoothPath(toPoints(samples, max));
	const area = `${line} L${width},${height} L0,${height} Z`;
	return (
		<svg
			viewBox={`0 0 ${width} ${height}`}
			preserveAspectRatio="none"
			className={className}
			role="img"
			aria-label="Download speed over the last 60 seconds"
		>
			<defs>
				<linearGradient id={gradientId} x1="0" x2="0" y1="0" y2="1">
					<stop offset="0%" stopColor="currentColor" stopOpacity="0.28" />
					<stop offset="100%" stopColor="currentColor" stopOpacity="0" />
				</linearGradient>
			</defs>
			{[0.25, 0.5, 0.75].map((ratio) => (
				<line
					key={ratio}
					x1="0"
					x2={width}
					y1={height * ratio}
					y2={height * ratio}
					stroke="currentColor"
					strokeOpacity="0.08"
					vectorEffect="non-scaling-stroke"
				/>
			))}
			<path d={area} fill={`url(#${gradientId})`} />
			<path
				d={line}
				fill="none"
				stroke="currentColor"
				strokeWidth="1.5"
				vectorEffect="non-scaling-stroke"
			/>
		</svg>
	);
}
