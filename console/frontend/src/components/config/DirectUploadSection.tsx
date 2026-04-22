import { useEffect, useRef } from 'react';
import { HardDrive, Layers } from 'lucide-react';
import { useFirmwareInfo } from '../../hooks/useFirmwareInfo';
import { useFirmwareUpload } from '../../hooks/useFirmwareUpload';
import { useSpiffsUpload } from '../../hooks/useSpiffsUpload';
import { SectionCard } from '@spoolhard/ui/components/SectionCard';
import { DropZone, type FileValidation } from '@spoolhard/ui/components/DropZone';
import { useReconnect } from '@spoolhard/ui/providers/ReconnectProvider';

// Console-specific sizes. SPIFFS partition is 3 MB on the console (partitions.csv),
// 1.94 MB on the scale — keep these in sync with the firmware side.
const ESP_IMAGE_MAGIC = 0xe9;
const APP_DESC_MAGIC  = 0xabcd5432;
const FIRMWARE_PARTITION_SIZE = 0x300000;   // 3 MB ota slot
const FRONTEND_PARTITION_SIZE = 0x300000;   // 3 MB SPIFFS (label still "spiffs")

async function readBytes(file: File, offset: number, length: number): Promise<Uint8Array> {
  const slice = file.slice(offset, offset + length);
  return new Uint8Array(await slice.arrayBuffer());
}

function u32le(buf: Uint8Array, offset: number): number {
  return buf[offset] | (buf[offset + 1] << 8) | (buf[offset + 2] << 16) | ((buf[offset + 3] << 24) >>> 0);
}

async function validateFirmware(file: File): Promise<FileValidation> {
  if (!file.name.endsWith('.bin')) {
    return { ok: false, error: 'Expected a .bin file' };
  }
  if (file.size < 256) {
    return { ok: false, error: 'File too small to be valid firmware' };
  }
  if (file.size > FIRMWARE_PARTITION_SIZE) {
    return { ok: false, error: `File exceeds ${(FIRMWARE_PARTITION_SIZE / 1024 / 1024).toFixed(0)} MB firmware partition limit` };
  }

  const header = await readBytes(file, 0, 48);

  if (header[0] !== ESP_IMAGE_MAGIC) {
    return { ok: false, error: 'Not an ESP32 firmware image (missing 0xE9 magic byte). Did you mean to drop this in the Frontend zone?' };
  }

  const segmentCount = header[1];
  if (segmentCount === 0 || segmentCount > 16) {
    return { ok: false, error: `Invalid segment count (${segmentCount}). File may be corrupted.` };
  }

  // Chip ID at offset 0x0C (2 bytes LE). ESP32-S3 = 0x0009 — both console
  // (WT32-SC01-Plus) and scale use the S3, so a single check covers both.
  const chipId = header[0x0c] | (header[0x0d] << 8);
  const chipNames: Record<number, string> = {
    0x0000: 'ESP32', 0x0002: 'ESP32-S2', 0x0005: 'ESP32-C3',
    0x0009: 'ESP32-S3', 0x000c: 'ESP32-C2', 0x000d: 'ESP32-C6',
  };
  const chipName = chipNames[chipId] || `Unknown (0x${chipId.toString(16)})`;

  if (chipId !== 0x0009) {
    return { ok: false, error: `Wrong target chip: ${chipName}. This device is ESP32-S3.` };
  }

  let info = `ESP32-S3 firmware · ${segmentCount} segments · ${(file.size / 1024).toFixed(0)} KB`;
  if (file.size >= 80) {
    const desc = await readBytes(file, 32, 48);
    const descMagic = u32le(desc, 0);
    if (descMagic === APP_DESC_MAGIC) {
      const versionBytes = desc.slice(16, 48);
      const nullIdx = versionBytes.indexOf(0);
      const version = new TextDecoder().decode(versionBytes.slice(0, nullIdx > 0 ? nullIdx : 32));
      if (version) info += ` · v${version}`;
    }
  }

  return { ok: true, info };
}

async function validateFrontend(file: File): Promise<FileValidation> {
  if (!file.name.endsWith('.bin')) {
    return { ok: false, error: 'Expected a .bin file' };
  }
  if (file.size < 256) {
    return { ok: false, error: 'File too small to be a valid frontend image' };
  }
  if (file.size > FRONTEND_PARTITION_SIZE) {
    return { ok: false, error: `File (${(file.size / 1024).toFixed(0)} KB) exceeds frontend partition size (${(FRONTEND_PARTITION_SIZE / 1024).toFixed(0)} KB)` };
  }

  const header = await readBytes(file, 0, 4);
  if (header[0] === ESP_IMAGE_MAGIC) {
    return { ok: false, error: 'This looks like a firmware image (starts with 0xE9), not a frontend bundle. Did you mean to drop this in the Backend zone?' };
  }

  return { ok: true, info: `Frontend image · ${(file.size / 1024).toFixed(0)} KB` };
}

export function DirectUploadSection() {
  const { data: fw } = useFirmwareInfo();
  const fwUpload     = useFirmwareUpload();
  const spiffsUpload = useSpiffsUpload();
  const reconnect    = useReconnect();

  // Any successful direct upload ends with ESP.restart() on the device side.
  // Kick off the reconnect watcher once per transition so the user gets a
  // persistent "Reconnecting…" overlay instead of a silent wait.
  const triggered = useRef(false);
  useEffect(() => {
    const firing = fwUpload.isSuccess || spiffsUpload.isSuccess;
    if (firing && !triggered.current) {
      triggered.current = true;
      reconnect.start(
        fwUpload.isSuccess ? 'Firmware flashed — rebooting…' : 'Frontend flashed — rebooting…'
      );
    }
    if (!fwUpload.isSuccess && !spiffsUpload.isSuccess) triggered.current = false;
  }, [fwUpload.isSuccess, spiffsUpload.isSuccess, reconnect]);

  return (
    <SectionCard
      title="Direct Upload"
      description="Flash firmware or frontend directly from your browser. The device will reboot after a successful upload."
    >
      <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
        <div className="space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-xs font-semibold uppercase tracking-wider text-text-secondary">Backend Firmware</span>
            {fw && <span className="font-mono text-[11px] text-text-muted">v{fw.fw_version}</span>}
          </div>
          <DropZone
            label="Backend firmware"
            icon={<HardDrive size={24} strokeWidth={1.5} />}
            validate={validateFirmware}
            onUpload={(file) => fwUpload.mutate(file)}
            disabled={spiffsUpload.isPending}
            isPending={fwUpload.isPending}
            isSuccess={fwUpload.isSuccess}
            progress={fwUpload.progress}
            error={fwUpload.error instanceof Error ? fwUpload.error.message : undefined}
          />
        </div>

        <div className="space-y-2">
          <div className="flex items-center justify-between">
            <span className="text-xs font-semibold uppercase tracking-wider text-text-secondary">Frontend</span>
            {fw && <span className="font-mono text-[11px] text-text-muted">v{fw.fe_version}</span>}
          </div>
          <DropZone
            label="Frontend image"
            icon={<Layers size={24} strokeWidth={1.5} />}
            validate={validateFrontend}
            onUpload={(file) => spiffsUpload.mutate(file)}
            disabled={fwUpload.isPending}
            isPending={spiffsUpload.isPending}
            isSuccess={spiffsUpload.isSuccess}
            progress={spiffsUpload.progress}
            error={spiffsUpload.error instanceof Error ? spiffsUpload.error.message : undefined}
          />
        </div>
      </div>

      {fw && (
        <div className="flex flex-wrap gap-x-4 gap-y-1 pt-1 font-mono text-[11px] text-text-muted">
          <span>Flash: {(fw.flash_size / 1024 / 1024).toFixed(1)} MB</span>
          <span>Frontend fs: {(fw.spiffs_used / 1024).toFixed(0)}/{(fw.spiffs_total / 1024).toFixed(0)} KB</span>
          <span>User fs: {(fw.userfs_used / 1024).toFixed(0)}/{(fw.userfs_total / 1024).toFixed(0)} KB</span>
          <span>Heap: {(fw.free_heap / 1024).toFixed(0)} KB free</span>
        </div>
      )}
    </SectionCard>
  );
}
