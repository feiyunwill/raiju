# Raiju — Design

The UI should feel like a first-party PS5 companion: monochrome, spacious, calm, with one clear focus at a time. Built on shadcn/ui, re-themed instead of re-invented.

> **Brand note:** We evoke the PS5 *aesthetic*, not Sony's identity. No PlayStation logos, no controller face-button symbols (△ ○ × □), no SST typeface, no "PlayStation" in the product name or title. Raiju has its own wordmark.

## Principles

1. **Black and white, color only for status.** Grayscale everywhere. The only hue is a muted red for errors.
2. **One hero per screen.** The current download's speed is the largest thing on Home. Everything else steps back.
3. **Focus is the interaction.** Like the PS5 home screen, focused items lift: white ring, slight scale. This works the same for keyboard, mouse hover and touch press.
4. **Quiet motion.** Smooth and short; nothing bounces.
5. **Mobile first.** It's a companion, so the phone is the primary device. On desktop the layout expands to use the extra space.

## Theme

Dark is the default (the console look). Light is the alternative (the companion-app look). Both are pure neutrals: chroma 0 for every token except `--destructive`.

```css
:root {                       /* light */
  --radius: 1rem;
  --background: oklch(1 0 0);
  --foreground: oklch(0.13 0 0);
  --card: oklch(0.97 0 0);
  --card-foreground: oklch(0.13 0 0);
  --popover: oklch(1 0 0);
  --popover-foreground: oklch(0.13 0 0);
  --primary: oklch(0.13 0 0);
  --primary-foreground: oklch(1 0 0);
  --secondary: oklch(0.94 0 0);
  --secondary-foreground: oklch(0.13 0 0);
  --muted: oklch(0.94 0 0);
  --muted-foreground: oklch(0.48 0 0);
  --accent: oklch(0.92 0 0);
  --accent-foreground: oklch(0.13 0 0);
  --destructive: oklch(0.58 0.19 25);
  --border: oklch(0.9 0 0);
  --input: oklch(0.9 0 0);
  --ring: oklch(0.13 0 0);
  --chart-1: oklch(0.13 0 0);   /* download */
  --chart-2: oklch(0.6 0 0);    /* upload   */
}

.dark {                       /* default */
  --background: oklch(0.08 0 0);
  --foreground: oklch(0.98 0 0);
  --card: oklch(0.14 0 0);
  --card-foreground: oklch(0.98 0 0);
  --popover: oklch(0.16 0 0);
  --popover-foreground: oklch(0.98 0 0);
  --primary: oklch(0.98 0 0);
  --primary-foreground: oklch(0.08 0 0);
  --secondary: oklch(0.2 0 0);
  --secondary-foreground: oklch(0.98 0 0);
  --muted: oklch(0.2 0 0);
  --muted-foreground: oklch(0.66 0 0);
  --accent: oklch(0.24 0 0);
  --accent-foreground: oklch(0.98 0 0);
  --destructive: oklch(0.65 0.18 25);
  --border: oklch(1 0 0 / 10%);
  --input: oklch(1 0 0 / 14%);
  --ring: oklch(0.98 0 0);
  --chart-1: oklch(0.98 0 0);
  --chart-2: oklch(0.55 0 0);
}
```

### Typography

- **Font:** Inter (variable, self-hosted, Latin subset only, to keep the bundle small). It's a clean humanist-geometric sans, close to the console's feel without copying its typeface.
- **Numbers:** always `font-variant-numeric: tabular-nums` so speeds and percentages don't jitter.
- **Weights:** Light 300 for hero numerals, Regular 400 for body, Medium 500 for labels and buttons. No bold.

| Role | Size / line height | Weight |
|---|---|---|
| Hero numeral (speed) | 64 / 64 (mobile 48) | 300 |
| Title | 28 / 34 | 400 |
| Section | 18 / 24 | 500 |
| Body | 15 / 22 | 400 |
| Caption / meta | 13 / 18 | 400, muted |

### Shape, space, elevation

- **Radius:** cards `--radius-xl` (≈22px); buttons, tabs, badges and inputs fully rounded (`rounded-full`), like the console's pills.
- **Spacing:** 4px base grid. Screen gutter 16px on mobile, 32px on desktop. Cards have 20px padding.
- **Elevation:** no drop shadows in dark mode; layers are separated by lightness (background → card → popover). Light mode gets one soft shadow for popovers only.

### Focus and motion

- **Focus / hover:** `ring-2 ring-ring ring-offset-2 ring-offset-background` plus `scale-[1.03]` on tiles and cards; buttons skip the scale. This is the signature interaction.
- **Durations:** 150ms for hover and focus, 250ms for sheets and dialogs. Easing: `cubic-bezier(0.2, 0, 0, 1)`.
- **Progress bars** animate their width smoothly with a 1s linear transition between polls, so they glide instead of stepping.
- `prefers-reduced-motion`: remove scale and slide; keep opacity fades.

### Cover art

- Covers (`icon0`) and key art (`pic0`) come from OrbisPatches (CUSA) and ProsperoPatches (PPSA), looked up by title ID through `/api/art/<TITLE_ID>` (the gateway on the PS5, a Vite middleware in development). The official title name from the same page replaces the name guessed from the file name.
- Artwork is the only colour in the UI. Key art sits behind the hero tile and the detail sheet header under a gradient into the surface colour, so text contrast never depends on the image.
- Without artwork (unknown title, lookup disabled in Settings, offline) the generated monochrome tile with initials is shown; layouts never shift between the two.

### Iconography

lucide-react (shadcn's default), stroke width 1.5, 20px default size. Icons are always monochrome.

## Screens

### Home
- **Header:** wordmark, centred pill navigation (Home · Downloads · Settings) on desktop, console status pill (`PS5 192.168.1.50`, pulsing dot when online), and the white "Add download ⌘K" pill. On phones the nav moves to a floating glass pill at the bottom.
- **Now downloading (hero tile):** cover art (generated monochrome tile with initials and title ID until real artwork is available), name, source file, kind badge, the speed as the largest numeral on screen, progress / remaining / connections, a progress bar labelled with the install name, a Pause/Resume pill and the stage stepper.
- **Throughput:** Vercel-style metric tabs (Speed, Peak 60s, Last minute) over a live 60-second area chart drawn in the foreground colour with hairline gridlines.
- **Internal storage:** free space as a large numeral, a segmented bar (games, Raiju staging, free) and a "Queue needs X · fits" pre-flight line.
- **Queue:** the first four jobs from the Downloads list, with "View all".

### Job stages (on every tile and row)
- A five-step stepper under the progress bar: **Download · Verify · Extract · Install · Ready**. Steps that don't apply (e.g. Extract for an `.ffpkg`) are hidden, not greyed out.
- The current step is white with a thin animated ring; completed steps are filled; upcoming steps are outlined.
- The progress bar always shows the *current* stage's progress. Its label changes with the stage: `Extracting · 38 % · 142 MB/s`.
- Kind badge next to the name: `IMAGE · ffpkg`, `ARCHIVE · 7z`, `FOLDER`. Outlined pill, monochrome.
- A name conflict pauses the job with an inline prompt on the tile: "`Game.ffpkg` already exists — Rename · Replace · Cancel".

### Downloads
- Pill tabs: **Active · Waiting · Done · Failed**, each with a count. Active includes jobs in Verify/Extract/Install.
- Rows: file icon, name, size, progress, speed, ETA, and a menu (Pause/Resume, Move up/down, Copy link, Remove, Remove + delete file).
- Toolbar buttons: Pause all / Resume all, and multi-select with bulk actions.

### Add Download (Drawer on mobile, Dialog on desktop)
- A textarea for one or more links (HTTP, FTP, magnet), plus a drop zone for `.torrent` files.
- Destination: preset pills (`Downloads`, `pkg`, `homebrew`) or "Browse…".
- Collapsible advanced options: connections, split, speed limit, rename, headers/cookies.
- After you paste a link, a probe row shows: kind badge, download size, extracted size, and a verdict: `ARCHIVE · 7z · 38 GB → 41 GB · needs 80 GB · fits (612 GB free)`, or a destructive warning if it doesn't fit.
- A title ID row, shown when one is detected in the link's name: `Will install as PPSA24003.exfat`, editable. Once `param.json` is read, the game's real name replaces the raw file name in the UI.
- An "After download" section: Install to (`homebrew` · `etaHEN/games` · custom), "Delete archive after extracting" switch (on by default).

### Download detail (Sheet)
- Overview, Files (with per-file selection for torrents), Connections/Peers, and Options (editable while active).

### Settings
- **Speed:** profile pills (Balanced / Max / Custom), connections per server, split, global limits.
- **Downloads:** default folder, concurrent downloads, file allocation.
- **Console:** keep awake while downloading, notifications, payload version and uptime.
- **Devices:** paired devices, revoke, show pairing code.
- **Appearance:** Dark / Light / System.

### Connection states
- **Pairing:** full-screen with a large 4-digit input: "Enter the code shown on your PS5".
- **Console unreachable:** a calm full-screen message: "Can't reach your PS5. Make sure it's on and Raiju is running." Retries automatically in the background.

## shadcn component map

| Need | Component |
|---|---|
| Actions | `Button` (pill variants: default, secondary, ghost, destructive) |
| Download tiles, panels | `Card` |
| Section switching | `Tabs` (restyled as pills) |
| Progress, storage meter | `Progress` |
| Add download | `Drawer` (mobile) / `Dialog` (desktop) |
| Task detail | `Sheet` |
| Row menus | `DropdownMenu` |
| Settings controls | `Switch`, `Input`, `Textarea`, pill radio groups (no `Slider`/`Select` yet) |
| Speed graph | `Chart` (Recharts area, `--chart-1/2`) |
| Notifications | `Sonner` |
| Pairing code | `InputOTP` (Radix `OneTimePasswordField`, no extra dependency) |
| Empty and loading states | `Empty`, `Skeleton` |
| Quick add (desktop) | `Command` (⌘K, paste a link) |
| Hints | `Tooltip` |

## Copy and voice

Short, plain, calm. Examples: "Paused", "Waiting for space", "Done — saved to pkg". Use sentence case everywhere. Never "Error!": instead, say what happened and what to do, e.g. "The server stopped responding. Retrying in 10s."

## Accessibility

- Text contrast ≥ 4.5:1 in both themes; the neutral tokens above meet this.
- Everything is reachable by keyboard. Arrow keys move focus within a tile row, like a controller would.
- Progress has `aria-valuenow`. Speed changes are not announced live; only completions and errors are.
- Touch targets ≥ 44px.
