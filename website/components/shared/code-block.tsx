"use client";

import * as React from "react";
import { Check, Copy } from "lucide-react";
import { cn } from "@/lib/utils";

export function CodeBlock({ code, label, className }: { code: string; label?: string; className?: string }) {
  const [copied, setCopied] = React.useState(false);

  const copy = async () => {
    await navigator.clipboard.writeText(code);
    setCopied(true);
    setTimeout(() => setCopied(false), 1500);
  };

  return (
    <div className={cn("overflow-hidden rounded-lg border border-border bg-[#0b1120]", className)}>
      <div className="flex items-center justify-between border-b border-white/10 px-4 py-2">
        <span className="text-xs font-medium text-slate-400">{label ?? "Terminal"}</span>
        <button
          onClick={copy}
          className="flex items-center gap-1.5 text-xs text-slate-400 transition-colors hover:text-slate-200"
          aria-label="Copy code"
        >
          {copied ? <Check className="h-3.5 w-3.5" /> : <Copy className="h-3.5 w-3.5" />}
          {copied ? "Copied" : "Copy"}
        </button>
      </div>
      <pre className="overflow-x-auto p-4 text-sm leading-relaxed text-slate-200">
        <code className="font-mono">{code}</code>
      </pre>
    </div>
  );
}
