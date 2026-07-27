export function LogoCloud({ names }: { names: string[] }) {
  return (
    <div className="grid grid-cols-2 gap-6 sm:grid-cols-3 lg:grid-cols-6">
      {names.map((name) => (
        <div
          key={name}
          className="flex h-14 items-center justify-center rounded-md border border-dashed border-border px-3 text-center text-sm font-medium text-muted-foreground grayscale"
        >
          {name}
        </div>
      ))}
    </div>
  );
}
