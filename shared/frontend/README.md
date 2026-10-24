# Shared Frontend Components

Reference copies of React components, providers, types, and theme that are shared between SpoolHard products (Scale, Console).

When building the Console frontend, copy these as the starting point and keep them in sync. When we add a third product, we'll migrate to npm workspaces with a proper `@spoolhard/ui` package.

## What's shared

- **`src/components/`** — Button, Card, Badge, StatusDot, InputField, PasswordField, SectionCard, DropZone, ConfirmDialog (accessible modal; `requireText` = type-to-confirm)
  plus ErrorBanner, QueryErrorBanner (retry for failed fetches), Skeleton/SkeletonRows, SettingsGroup (collapsible `<details>` group), and the cross-product sections DeviceNameSection, DeviceControlSection (`factoryResetMessage` prop), BackupRestoreSection (`product` prop)
- **`src/hooks/`** — useDeviceName, useDeviceControl (identical endpoints on both products)
- **`src/providers/`** — QueryProvider, WebSocketProvider
- **`src/types/`** — WebSocket message types
- **`src/app.css`** — TailwindCSS v4 theme (@theme block with all design tokens)

## What's NOT shared (product-specific)

- Dashboard components (StatsRow, EventLog, etc.)
- Config sections (WifiSection, ScaleSection, etc.)
- Layout components (Header, NavTabs)
- API hooks (endpoints differ per product)
- Pages
- App.tsx, main.tsx
