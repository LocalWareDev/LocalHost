import type { Metadata } from "next";
import { Inter, JetBrains_Mono } from "next/font/google";
import "./globals.css";
import { cn } from "@/lib/utils";
import { siteConfig } from "@/lib/site-config";
import { ThemeProvider } from "@/components/layout/theme-provider";
import { AnnouncementBanner } from "@/components/layout/announcement-banner";
import { Navbar } from "@/components/layout/navbar";
import { Footer } from "@/components/layout/footer";
import { CookieConsent } from "@/components/layout/cookie-consent";
import { JsonLd } from "@/components/shared/json-ld";

const inter = Inter({ subsets: ["latin"], variable: "--font-inter", display: "swap" });
const mono = JetBrains_Mono({ subsets: ["latin"], variable: "--font-mono", display: "swap" });

export const metadata: Metadata = {
  metadataBase: new URL(siteConfig.url),
  title: {
    default: `${siteConfig.name} — ${siteConfig.tagline}`,
    template: `%s — ${siteConfig.name}`,
  },
  description: siteConfig.description,
  keywords: [
    "enterprise virtualization",
    "hypervisor",
    "virtual machine platform",
    "fleet management",
    "LocalHost",
    "LocalWare",
  ],
  authors: [{ name: siteConfig.company }],
  creator: siteConfig.company,
  openGraph: {
    type: "website",
    locale: "en_US",
    url: siteConfig.url,
    title: `${siteConfig.name} — ${siteConfig.tagline}`,
    description: siteConfig.description,
    siteName: siteConfig.name,
  },
  twitter: {
    card: "summary_large_image",
    title: `${siteConfig.name} — ${siteConfig.tagline}`,
    description: siteConfig.description,
  },
  robots: {
    index: true,
    follow: true,
  },
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning>
      <body className={cn(inter.variable, mono.variable, "font-sans")}>
        <JsonLd
          data={{
            "@context": "https://schema.org",
            "@type": "Organization",
            name: siteConfig.company,
            url: siteConfig.url,
            brand: {
              "@type": "Brand",
              name: siteConfig.name,
            },
          }}
        />
        <ThemeProvider attribute="class" defaultTheme="light" enableSystem disableTransitionOnChange>
          <div className="relative flex min-h-screen flex-col">
            <AnnouncementBanner />
            <Navbar />
            <main className="flex-1">{children}</main>
            <Footer />
          </div>
          <CookieConsent />
        </ThemeProvider>
      </body>
    </html>
  );
}
