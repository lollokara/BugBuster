// =============================================================================
// api/types.ts — shared domain types for all API modules
// =============================================================================

export interface DeviceInfo {
  siliconRev: number;
  siliconId0: string;
  siliconId1: string;
  macAddress: string;
  spiOk: boolean;
}

export interface PairingInfo {
  macAddress: string;
  tokenFingerprint: string | null;
  transport: "http" | "usb";
}

export interface SelftestSuppliesCached {
  available: boolean;
  timestampMs: number;
  rails: Array<{ rail: number; name: string; voltageV: number }>;
}

export interface ScriptStatus {
  running?: boolean;
  currentScriptId?: number;
  totalRuns?: number;
  totalErrors?: number;
  lastError?: string;
  mode?: string;
  globalsBytes?: number;
  globalsCount?: number;
  autoResetCount?: number;
  lastEvalAtMs?: number;
  idleForMs?: number;
  watermarkSoftHit?: boolean;
}

export interface ScriptStorageStatus {
  totalBytes: number;
  usedBytes: number;
  freeBytes: number;
  scriptCount: number;
  maxScriptBytes: number;
  maxScripts: number;
}

export interface AutorunStatus {
  enabled?: boolean;
  has_script?: boolean;
  hasScript?: boolean;
  io12_high?: boolean;
  io12High?: boolean;
  last_run_ok?: boolean;
  lastRunOk?: boolean;
  last_run_id?: number;
  lastRunId?: number;
  name?: string;
}

export interface SelftestStatus {
  boot?: {
    ran?: boolean;
    passed?: boolean;
    vadj1V?: number;
    vadj2V?: number;
    vlogicV?: number;
  };
  calibration?: {
    status?: number;
    channel?: number;
    points?: number;
    lastVoltageV?: number;
    errorMv?: number;
  };
  workerEnabled?: boolean;
  supplyMonitorActive?: boolean;
}

export interface QuickSetupSummary {
  index?: number;
  occupied?: boolean;
  summary?: unknown;
  name?: string;
  ts?: number;
  timestamp?: number;
  updatedAt?: string;
}

export interface QuickSetupList {
  slots: QuickSetupSummary[];
}

export type QuickSetupPayload = Record<string, unknown>;

export interface QuickSetupApplyResult {
  ok?: boolean;
  applied?: unknown;
  failed?: string[];
}

export interface BoardRail {
  value: number;
  locked: boolean;
}

export interface BoardProfile {
  id: string;
  name: string;
  description: string;
  rails: { vlogic: BoardRail; vadj1: BoardRail; vadj2: BoardRail };
  pinCount: number;
}

export interface BoardState {
  active: string | null;
  available: BoardProfile[];
}

export type WavegenType = "sine" | "square" | "triangle" | "sawtooth";

export interface OtaPartition {
  label: string;
  address: number;
  size: number;
  state?: string;
}

export interface OtaInfo {
  running?: OtaPartition;
  next?: OtaPartition;
  lastInvalid?: OtaPartition;
  canRollback: boolean;
  fwMajor: number;
  fwMinor: number;
  fwPatch: number;
}

export interface OtaUploadResult {
  success: boolean;
  bytesWritten?: number;
  partition?: string;
  sha256Verified?: boolean;
}
