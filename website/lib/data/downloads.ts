export interface ReleaseChannel {
  channel: "Stable" | "Beta" | "Nightly";
  version: string;
  date: string;
  description: string;
}

export const releaseChannels: ReleaseChannel[] = [
  {
    channel: "Stable",
    version: "2.4.0",
    date: "2026-06-10",
    description: "Recommended for production use. Fully qualified across all supported platforms.",
  },
  {
    channel: "Beta",
    version: "2.5.0-beta.3",
    date: "2026-07-15",
    description: "Feature-complete preview of the next release. Suitable for staging environments.",
  },
  {
    channel: "Nightly",
    version: "2.5.0-nightly.20260727",
    date: "2026-07-27",
    description: "Automated build from the latest development branch. Not recommended for production.",
  },
];

export interface VersionHistoryEntry {
  version: string;
  date: string;
  type: "Major" | "Minor" | "Patch" | "Security";
  summary: string;
}

export const versionHistory: VersionHistoryEntry[] = [
  { version: "2.4.0", date: "2026-06-10", type: "Minor", summary: "Sub-second live migration, VM-level RBAC scoping, rewritten CLI." },
  { version: "2.3.2", date: "2026-07-02", type: "Security", summary: "Backport fix for synthetic NIC timing side-channel (CVE pending)." },
  { version: "2.3.1", date: "2026-04-18", type: "Patch", summary: "Stability fixes for distributed storage rebuild under host failure." },
  { version: "2.3.0", date: "2026-02-27", type: "Minor", summary: "NUMA-aware scheduling, Terraform provider beta." },
  { version: "2.2.0", date: "2025-11-12", type: "Minor", summary: "Directory integration (SAML/OIDC), compliance reporting exports." },
  { version: "2.1.0", date: "2025-08-05", type: "Major", summary: "Distributed, erasure-coded storage architecture introduced." },
];

export const downloadPlatforms = [
  { name: "Windows", format: "Installer (.exe)", requirement: "64-bit, Windows 10 21H2+" },
  { name: "macOS (Apple Silicon)", format: "Disk image (.dmg)", requirement: "macOS 13 Ventura+" },
  { name: "macOS (Intel)", format: "Disk image (.dmg)", requirement: "macOS 13 Ventura+" },
  { name: "Linux", format: ".deb / .rpm / AppImage", requirement: "Kernel 5.10+, glibc 2.31+" },
];

export const checksumSample = `SHA256 (LocalHost-2.4.0-win64.exe)   = 8f14e45fceea167a5a36dedd4bea2543
SHA256 (LocalHost-2.4.0-macos-arm64.dmg) = c99a74c555371a1846e07d2603d1e3c
SHA256 (LocalHost-2.4.0-macos-x64.dmg)   = 45c48cce2e2d7fbdea1afc51c7c6ad26
SHA256 (localhost_2.4.0_amd64.deb)   = 6512bd43d9caa6e02c990b0a82652dca`;
