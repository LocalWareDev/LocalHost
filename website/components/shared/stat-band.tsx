export interface Stat {
  value: string;
  label: string;
}

export function StatBand({ stats }: { stats: Stat[] }) {
  return (
    <div className="grid grid-cols-2 gap-8 border-y border-border py-10 sm:grid-cols-4">
      {stats.map((stat) => (
        <div key={stat.label} className="text-center sm:text-left">
          <div className="text-3xl font-semibold tracking-tight sm:text-4xl">{stat.value}</div>
          <div className="mt-1 text-sm text-muted-foreground">{stat.label}</div>
        </div>
      ))}
    </div>
  );
}
