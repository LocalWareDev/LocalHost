export const siteConfig = {
  name: "LocalHost",
  company: "LocalWare Corporation",
  tagline: "Enterprise Virtualization. Built for Performance.",
  description:
    "LocalHost is an enterprise virtualization platform for organizations that require reliability, security, and performance at scale — from single workstations to global fleets.",
  url: "https://localhost.localware.com",
  ogImage: "/og-image.png",
  links: {
    github: "https://github.com/localware",
    twitter: "https://twitter.com/localwarecorp",
    linkedin: "https://linkedin.com/company/localware",
  },
} as const;

export type SiteConfig = typeof siteConfig;
