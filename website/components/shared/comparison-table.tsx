import * as React from "react";
import { Check, Minus } from "lucide-react";
import { Table, TableBody, TableCell, TableHead, TableHeader, TableRow } from "@/components/ui/table";
import type { ComparisonRow } from "@/lib/data/pricing";

function Cell({ value }: { value: string | boolean }) {
  if (typeof value === "boolean") {
    return value ? (
      <Check className="mx-auto h-4 w-4 text-primary" />
    ) : (
      <Minus className="mx-auto h-4 w-4 text-muted-foreground/40" />
    );
  }
  return <span className="text-sm">{value}</span>;
}

export function ComparisonTable({
  groups,
  headers = ["Community", "Professional", "Enterprise"],
}: {
  groups: { category: string; rows: ComparisonRow[] }[];
  headers?: [string, string, string] | string[];
}) {
  return (
    <div className="overflow-hidden rounded-lg border border-border">
      <Table>
        <TableHeader>
          <TableRow>
            <TableHead className="w-1/2">Feature</TableHead>
            {headers.map((h) => (
              <TableHead key={h} className="text-center">
                {h}
              </TableHead>
            ))}
          </TableRow>
        </TableHeader>
        <TableBody>
          {groups.map((group) => (
            <React.Fragment key={group.category}>
              <TableRow className="bg-muted/40 hover:bg-muted/40">
                <TableCell colSpan={4} className="py-2.5 text-xs font-semibold uppercase tracking-wide text-muted-foreground">
                  {group.category}
                </TableCell>
              </TableRow>
              {group.rows.map((row) => (
                <TableRow key={row.feature}>
                  <TableCell className="text-sm">{row.feature}</TableCell>
                  <TableCell className="text-center">
                    <Cell value={row.community} />
                  </TableCell>
                  <TableCell className="text-center">
                    <Cell value={row.professional} />
                  </TableCell>
                  <TableCell className="text-center">
                    <Cell value={row.enterprise} />
                  </TableCell>
                </TableRow>
              ))}
            </React.Fragment>
          ))}
        </TableBody>
      </Table>
    </div>
  );
}
