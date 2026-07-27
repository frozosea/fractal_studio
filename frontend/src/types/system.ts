// System types — hardware info and capability checks.

export interface Hardware {
  cpuModel: string;
  cpuLogicalCores: number;
  cpuPhysicalCores: number;
  memoryTotalMiB: number;
  memoryAvailableMiB: number;
  gpuModel: string;
  gpuMemory: string;
}

export interface SystemCheckResult {
  openmp: boolean;
  cuda: boolean;
}
