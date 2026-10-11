import { cn } from "cn";
import { unstable_OneTimePasswordField as OneTimePasswordField } from "radix-ui";
import type * as React from "react";

type InputOTPProps = React.ComponentProps<typeof OneTimePasswordField.Root> & {
	length?: number;
};

/** Code input styled as the console's large pairing field, with one cell per digit. */
export function InputOTP({ length = 4, className, ...props }: InputOTPProps) {
	const cells = Array.from({ length }, (_, position) => `cell-${position}`);
	return (
		<OneTimePasswordField.Root
			data-slot="input-otp"
			className={cn("flex items-center gap-3", className)}
			{...props}
		>
			{cells.map((cell, index) => (
				<OneTimePasswordField.Input
					key={cell}
					index={index}
					data-slot="input-otp-slot"
					className="border-input bg-foreground/[0.04] focus-visible:ring-ring focus-visible:ring-offset-background h-16 w-12 rounded-2xl border text-center text-3xl font-light tabular-nums outline-none transition-colors duration-150 ease-raiju focus-visible:ring-2 focus-visible:ring-offset-2 aria-invalid:border-destructive"
				/>
			))}
			<OneTimePasswordField.HiddenInput />
		</OneTimePasswordField.Root>
	);
}
