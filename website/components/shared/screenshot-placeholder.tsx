import { cn } from "@/lib/utils";

export function ScreenshotPlaceholder({ label, className }: { label: string; className?: string }) {
  return (
    <div
      className={cn(
        "overflow-hidden rounded-xl border border-border bg-card shadow-sm",
        className
      )}
    >
      <div className="flex items-center gap-1.5 border-b border-border bg-muted/50 px-4 py-2.5">
        <span className="h-2.5 w-2.5 rounded-full bg-destructive/60" />
        <span className="h-2.5 w-2.5 rounded-full bg-amber-500/60" />
        <span className="h-2.5 w-2.5 rounded-full bg-emerald-500/60" />
        <span className="ml-3 text-xs text-muted-foreground">{label}</span>
      </div>
      <div className="bg-grid-slate grid grid-cols-4 gap-3 bg-muted/20 p-6">
        <div className="col-span-1 space-y-2">
          <div className="h-3 w-3/4 rounded bg-foreground/10" />
          <div className="h-2.5 w-full rounded bg-foreground/10" />
          <div className="h-2.5 w-full rounded bg-foreground/10" />
          <div className="h-2.5 w-2/3 rounded bg-foreground/10" />
        </div>
        <div className="col-span-3 space-y-3">
          <div className="h-24 rounded-md border border-border bg-background/60" />
          <div className="grid grid-cols-3 gap-3">
            <div className="h-16 rounded-md border border-border bg-background/60" />
            <div className="h-16 rounded-md border border-border bg-background/60" />
            <div className="h-16 rounded-md border border-border bg-background/60" />
          </div>
        </div>
      </div>
    </div>
  );
}
