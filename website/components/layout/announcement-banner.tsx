"use client";

import * as React from "react";
import Link from "next/link";
import { ArrowRight, X } from "lucide-react";

export function AnnouncementBanner() {
  const [dismissed, setDismissed] = React.useState(false);

  if (dismissed) return null;

  return (
    <div className="relative flex items-center justify-center gap-2 bg-primary px-4 py-2.5 text-center text-sm font-medium text-primary-foreground">
      <Link href="/blog/localhost-2-4-live-migration" className="inline-flex items-center gap-1.5 hover:underline">
        LocalHost 2.4 introduces sub-second live migration
        <ArrowRight className="h-3.5 w-3.5" />
      </Link>
      <button
        onClick={() => setDismissed(true)}
        aria-label="Dismiss announcement"
        className="absolute right-3 top-1/2 -translate-y-1/2 rounded p-1 hover:bg-white/10"
      >
        <X className="h-3.5 w-3.5" />
      </button>
    </div>
  );
}
