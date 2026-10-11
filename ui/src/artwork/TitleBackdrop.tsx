import { cn } from "cn";
import { useState } from "react";
import { useArtwork } from "./useArtwork";

type TitleBackdropProps = {
	titleId: string;
	className?: string;
};

/** The title's key art (pic0) as a soft background that fades into the surface below it. */
export function TitleBackdrop({ titleId, className }: TitleBackdropProps) {
	const artwork = useArtwork(titleId);
	const [loaded, setLoaded] = useState<string | null>(null);
	const background = artwork?.background;
	if (!background) {
		return null;
	}
	return (
		<div
			aria-hidden="true"
			className={cn(
				"pointer-events-none absolute inset-0 overflow-hidden",
				className,
			)}
		>
			<img
				src={background}
				alt=""
				decoding="async"
				referrerPolicy="no-referrer"
				onLoad={() => setLoaded(background)}
				className={cn(
					"size-full scale-105 object-cover transition-opacity duration-700",
					loaded === background ? "opacity-100" : "opacity-0",
				)}
			/>
			<div className="from-card via-card/75 to-card/20 absolute inset-0 bg-gradient-to-t md:bg-gradient-to-r" />
			<div className="from-card/90 absolute inset-0 bg-gradient-to-t to-transparent to-50%" />
		</div>
	);
}

/** Official title name when artwork lookup found one, else the given fallback. */
export function useTitleName(titleId: string, fallback: string): string {
	return useArtwork(titleId)?.name ?? fallback;
}
