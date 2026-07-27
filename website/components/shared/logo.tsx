import { Boxes } from "lucide-react";
import { cn } from "@/lib/utils";

export function Logo({ className }: { className?: string }) {
  return (
    <span className={cn("inline-flex items-center gap-2 font-semibold tracking-tight", className)}>
      <span className="flex h-7 w-7 items-center justify-center rounded-md bg-primary text-primary-foreground">
        <Boxes className="h-4 w-4" strokeWidth={2.25} />
      </span>
      <span className="text-base">
        LocalHost<span className="align-super text-[0.55em]">™</span>
      </span>
    </span>
  );
}
