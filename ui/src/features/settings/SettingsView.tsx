import { cn } from "cn";
import type * as React from "react";
import { useEffect, useState } from "react";
import { useArtworkEnabled } from "@/artwork/useArtwork";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Switch } from "@/components/ui/switch";
import { fileAllocationModes } from "@/data/fileAllocation";
import type {
	Console,
	DashboardActions,
	Device,
	FolderRoot,
	Settings,
	SourceMode,
} from "@/data/types";
import { formatDuration } from "@/lib/format";
import { ThemeToggle } from "@/theme/ThemeToggle";

const profiles: {
	id: Settings["speed"]["profile"];
	label: string;
	connectionsPerServer: number;
	split: number;
}[] = [
	{ id: "balanced", label: "Balanced", connectionsPerServer: 16, split: 16 },
	{ id: "max", label: "Max", connectionsPerServer: 64, split: 64 },
	{ id: "custom", label: "Custom", connectionsPerServer: 16, split: 16 },
];

function Row({
	label,
	children,
}: {
	label: string;
	children: React.ReactNode;
}) {
	return (
		<div className="flex min-h-14 items-center justify-between gap-4 px-5 py-3">
			<span className="text-[15px]">{label}</span>
			<span className="text-muted-foreground flex items-center gap-2 text-[13px]">
				{children}
			</span>
		</div>
	);
}

function Pills<T extends string>({
	name,
	value,
	options,
	onChange,
}: {
	name: string;
	value: T;
	options: { id: T; label: string }[];
	onChange: (next: T) => void;
}) {
	return (
		<div className="flex flex-wrap gap-2">
			{options.map((option) => (
				<label
					key={option.id}
					className={cn(
						"flex h-9 cursor-pointer items-center rounded-full px-4 text-xs transition-colors duration-150",
						"has-[:focus-visible]:ring-ring has-[:focus-visible]:ring-offset-background has-[:focus-visible]:ring-2 has-[:focus-visible]:ring-offset-2",
						value === option.id
							? "bg-primary text-primary-foreground"
							: "bg-foreground/[0.05] text-muted-foreground hover:text-foreground",
					)}
				>
					<input
						type="radio"
						name={name}
						value={option.id}
						checked={value === option.id}
						onChange={() => onChange(option.id)}
						className="sr-only"
					/>
					{option.label}
				</label>
			))}
		</div>
	);
}

function NumberField({
	value,
	min,
	max,
	onChange,
	label,
}: {
	value: number;
	min: number;
	max: number;
	onChange: (next: number) => void;
	label: string;
}) {
	return (
		<Input
			type="number"
			min={min}
			max={max}
			value={value}
			aria-label={label}
			onChange={(event) => {
				const next = Number(event.target.value);
				if (Number.isFinite(next)) {
					onChange(Math.min(max, Math.max(min, next)));
				}
			}}
			className="w-20 rounded-full text-center font-mono"
		/>
	);
}

/** Engine tuning, download defaults, console, paired devices and appearance. */
export function SettingsView({
	console,
	mode,
	folders,
	actions,
}: {
	console: Console;
	mode: SourceMode;
	folders: FolderRoot[];
	actions: DashboardActions;
}) {
	const [artwork, setArtwork] = useArtworkEnabled();
	const [settings, setSettings] = useState<Settings | null>(null);
	const [devices, setDevices] = useState<Device[]>([]);
	const { loadSettings, loadDevices, saveSettings, revokeDevice } = actions;

	useEffect(() => {
		let cancelled = false;
		void loadSettings().then((loaded) => {
			if (!cancelled) {
				setSettings(loaded);
			}
		});
		void loadDevices().then((loaded) => {
			if (!cancelled) {
				setDevices(loaded);
			}
		});
		return () => {
			cancelled = true;
		};
	}, [loadSettings, loadDevices]);

	const save = async (next: Settings) => {
		setSettings(next);
		if (!(await saveSettings(next))) {
			setSettings(await loadSettings());
		}
	};

	const revoke = async (id: string) => {
		await revokeDevice(id);
		setDevices(await loadDevices());
	};

	return (
		<div className="flex max-w-2xl flex-col gap-6">
			<h1 className="text-[28px] leading-[34px] font-normal tracking-tight">
				Settings
			</h1>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Speed</h2>
				<div className="surface divide-border divide-y">
					<div className="flex flex-col gap-3 px-5 py-4">
						<span className="text-[15px]">Profile</span>
						<Pills
							name="speed-profile"
							value={settings?.speed.profile ?? "balanced"}
							options={profiles}
							onChange={(id) => {
								if (!settings) {
									return;
								}
								const preset = profiles.find((item) => item.id === id);
								void save({
									...settings,
									speed: {
										...settings.speed,
										profile: id,
										...(id === "custom"
											? {}
											: {
													connectionsPerServer:
														preset?.connectionsPerServer ?? 16,
													split: preset?.split ?? 16,
												}),
									},
								});
							}}
						/>
					</div>
					<Row label="Connections per server">
						<NumberField
							label="Connections per server"
							value={settings?.speed.connectionsPerServer ?? 16}
							min={1}
							max={64}
							onChange={(value) =>
								settings &&
								void save({
									...settings,
									speed: {
										...settings.speed,
										profile: "custom",
										connectionsPerServer: value,
									},
								})
							}
						/>
					</Row>
					<Row label="Split">
						<NumberField
							label="Split"
							value={settings?.speed.split ?? 16}
							min={1}
							max={64}
							onChange={(value) =>
								settings &&
								void save({
									...settings,
									speed: { ...settings.speed, profile: "custom", split: value },
								})
							}
						/>
					</Row>
					<Row label="Download limit">
						<NumberField
							label="Download limit in MB/s"
							value={Math.round(
								(settings?.speed.downloadLimit ?? 0) / 1_000_000,
							)}
							min={0}
							max={1000}
							onChange={(value) =>
								settings &&
								void save({
									...settings,
									speed: {
										...settings.speed,
										downloadLimit: value * 1_000_000,
									},
								})
							}
						/>
						MB/s
					</Row>
					<Row label="Upload limit">
						<NumberField
							label="Upload limit in MB/s"
							value={Math.round((settings?.speed.uploadLimit ?? 0) / 1_000_000)}
							min={0}
							max={1000}
							onChange={(value) =>
								settings &&
								void save({
									...settings,
									speed: { ...settings.speed, uploadLimit: value * 1_000_000 },
								})
							}
						/>
						MB/s
					</Row>
				</div>
			</section>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Downloads</h2>
				<div className="surface divide-border divide-y">
					<div className="flex flex-col gap-3 px-5 py-4">
						<span className="text-[15px]">Default folder</span>
						<Pills
							name="download-folder"
							value={settings?.downloads.directory ?? folders[0]?.path ?? ""}
							options={folders.map((root) => ({
								id: root.path,
								label: root.label,
							}))}
							onChange={(path) =>
								settings &&
								void save({
									...settings,
									downloads: { ...settings.downloads, directory: path },
								})
							}
						/>
					</div>
					<Row label="Concurrent downloads">
						<NumberField
							label="Concurrent downloads"
							value={settings?.downloads.concurrent ?? 5}
							min={1}
							max={16}
							onChange={(value) =>
								settings &&
								void save({
									...settings,
									downloads: { ...settings.downloads, concurrent: value },
								})
							}
						/>
					</Row>
					<Row label="File allocation">
						<Pills
							name="file-allocation"
							value={settings?.downloads.fileAllocation ?? "none"}
							options={fileAllocationModes}
							onChange={(id) =>
								settings &&
								void save({
									...settings,
									downloads: { ...settings.downloads, fileAllocation: id },
								})
							}
						/>
					</Row>
				</div>
			</section>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Console</h2>
				<div className="surface divide-border divide-y">
					<Row label="Keep awake while downloading">
						<Switch
							checked={settings?.console.keepAwake ?? true}
							onCheckedChange={(on) =>
								settings &&
								void save({
									...settings,
									console: { ...settings.console, keepAwake: on },
								})
							}
							aria-label="Keep awake while downloading"
						/>
					</Row>
					<Row label="Notifications">
						<Switch
							checked={settings?.console.notifications ?? true}
							onCheckedChange={(on) =>
								settings &&
								void save({
									...settings,
									console: { ...settings.console, notifications: on },
								})
							}
							aria-label="Show notifications when a job is ready"
						/>
					</Row>
					<Row label="Address">
						<span className="font-mono">{console.host}</span>
					</Row>
					<Row label="Version">{console.version || "—"}</Row>
					<Row label="Engine">{console.engine || "—"}</Row>
					<Row label="Firmware">
						<span className="font-mono">{console.firmware || "—"}</span>
					</Row>
					<Row label="Uptime">
						{console.uptimeSeconds === null
							? "—"
							: formatDuration(console.uptimeSeconds)}
					</Row>
					<Row label="Data">
						{mode === "demo"
							? "Demo data"
							: console.online
								? "Live"
								: "Offline"}
					</Row>
				</div>
			</section>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Devices</h2>
				<div className="surface divide-border divide-y">
					{devices.map((device) => (
						<Row
							key={device.id}
							label={device.label + (device.current ? " · this browser" : "")}
						>
							<Button
								size="sm"
								variant="ghost"
								aria-label={`Revoke ${device.label}`}
								onClick={() => void revoke(device.id)}
							>
								Revoke
							</Button>
						</Row>
					))}
					<p className="text-muted-foreground px-5 py-4 text-[13px]">
						The payload shows a pairing code on your PS5 every time it starts.
						Revoking a device means it has to pair again.
					</p>
				</div>
			</section>
			<section className="flex flex-col gap-2">
				<h2 className="text-muted-foreground px-1 text-[13px]">Appearance</h2>
				<div className="surface divide-border divide-y">
					<Row label="Theme">
						<ThemeToggle />
					</Row>
					<Row label="Cover art">
						<Switch
							checked={artwork}
							onCheckedChange={setArtwork}
							aria-label="Show cover art"
						/>
					</Row>
				</div>
			</section>
		</div>
	);
}
