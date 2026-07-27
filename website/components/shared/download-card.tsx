import { Download, Monitor } from "lucide-react";
import { Button } from "@/components/ui/button";
import { Card, CardContent } from "@/components/ui/card";

export function DownloadCard({
  platform,
  format,
  size,
  requirement,
}: {
  platform: string;
  format: string;
  size?: string;
  requirement?: string;
}) {
  return (
    <Card className="transition-colors hover:border-primary/40">
      <CardContent className="flex items-center justify-between gap-4 p-5">
        <div className="flex items-center gap-3">
          <div className="flex h-10 w-10 shrink-0 items-center justify-center rounded-md bg-primary/10 text-primary">
            <Monitor className="h-5 w-5" />
          </div>
          <div>
            <p className="text-sm font-semibold">{platform}</p>
            <p className="text-xs text-muted-foreground">
              {format}
              {size ? ` · ${size}` : ""}
            </p>
            {requirement && <p className="mt-0.5 text-xs text-muted-foreground">{requirement}</p>}
          </div>
        </div>
        <Button size="sm" variant="outline" className="shrink-0 gap-1.5">
          <Download className="h-3.5 w-3.5" />
          Download
        </Button>
      </CardContent>
    </Card>
  );
}
