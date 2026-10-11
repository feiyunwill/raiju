import { Zap } from "lucide-react";
import { useState } from "react";
import { Button } from "@/components/ui/button";
import { InputOTP } from "@/components/ui/input-otp";

type PairingScreenProps = {
	notice: string | null;
	onPair: (code: string) => Promise<boolean>;
};

/** Full-screen pairing: the 4-digit code the payload shows on the console. */
export function PairingScreen({ notice, onPair }: PairingScreenProps) {
	const [code, setCode] = useState("");
	const [busy, setBusy] = useState(false);

	const submit = async (value: string) => {
		if (value.length !== 4 || busy) {
			return;
		}
		setBusy(true);
		const paired = await onPair(value);
		setBusy(false);
		if (!paired) {
			setCode("");
		}
	};

	return (
		<main className="mx-auto flex min-h-dvh max-w-md flex-col items-center justify-center gap-8 px-6 text-center">
			<div className="flex flex-col items-center gap-3">
				<span className="bg-foreground text-background flex size-12 items-center justify-center rounded-2xl">
					<Zap className="size-6" strokeWidth={2} fill="currentColor" />
				</span>
				<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
					Enter the code shown on your PS5
				</h1>
				<p className="text-muted-foreground max-w-sm text-[15px]">
					Raiju shows a 4-digit code when the payload starts. Pairing remembers
					this browser, so you only do it once.
				</p>
			</div>
			<InputOTP
				autoFocus
				length={4}
				value={code}
				onValueChange={setCode}
				autoSubmit
				onAutoSubmit={(value) => void submit(value)}
				aria-label="Pairing code"
			/>
			<div className="flex flex-col items-center gap-3">
				<Button
					onClick={() => void submit(code)}
					disabled={code.length !== 4 || busy}
				>
					{busy ? "Pairing…" : "Pair this browser"}
				</Button>
				{notice && (
					<p role="alert" className="text-destructive text-[13px]">
						{notice}
					</p>
				)}
			</div>
		</main>
	);
}
