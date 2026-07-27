import { SectionHeading } from "@/components/shared/section-heading";

const hostOS = ["Windows 10 / 11", "macOS 13+ (Apple Silicon & Intel)", "Ubuntu / Debian / Fedora / RHEL"];
const guestOS = [
  "Windows Server & Desktop",
  "Ubuntu, Debian, Fedora, RHEL, SUSE",
  "FreeBSD & OpenBSD",
  "Legacy OS support for compatibility testing",
];

export function OsCompatibility() {
  return (
    <section className="border-y border-border bg-muted/20 py-20">
      <div className="container">
        <SectionHeading
          eyebrow="Compatibility"
          title="Runs where your team already works"
          description="LocalHost supports the host and guest operating systems enterprise environments actually run."
          className="mb-12"
        />
        <div className="grid grid-cols-1 gap-6 sm:grid-cols-2">
          <div className="rounded-lg border border-border bg-card p-6">
            <h3 className="font-semibold">Host operating systems</h3>
            <ul className="mt-4 space-y-2.5 text-sm text-muted-foreground">
              {hostOS.map((os) => (
                <li key={os} className="border-b border-border/60 pb-2.5 last:border-0 last:pb-0">
                  {os}
                </li>
              ))}
            </ul>
          </div>
          <div className="rounded-lg border border-border bg-card p-6">
            <h3 className="font-semibold">Guest operating systems</h3>
            <ul className="mt-4 space-y-2.5 text-sm text-muted-foreground">
              {guestOS.map((os) => (
                <li key={os} className="border-b border-border/60 pb-2.5 last:border-0 last:pb-0">
                  {os}
                </li>
              ))}
            </ul>
          </div>
        </div>
      </div>
    </section>
  );
}
