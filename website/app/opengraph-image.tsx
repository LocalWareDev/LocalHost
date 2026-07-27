import { ImageResponse } from "next/og";
import { siteConfig } from "@/lib/site-config";

export const size = { width: 1200, height: 630 };
export const contentType = "image/png";

export default function OpengraphImage() {
  return new ImageResponse(
    (
      <div
        style={{
          width: "100%",
          height: "100%",
          display: "flex",
          flexDirection: "column",
          justifyContent: "center",
          padding: "80px",
          background: "#0b0f19",
          color: "white",
          fontFamily: "sans-serif",
        }}
      >
        <div style={{ display: "flex", alignItems: "center", gap: 16 }}>
          <div
            style={{
              width: 48,
              height: 48,
              borderRadius: 10,
              background: "#345cf0",
              display: "flex",
              alignItems: "center",
              justifyContent: "center",
              fontSize: 26,
              fontWeight: 700,
            }}
          >
            L
          </div>
          <div style={{ fontSize: 32, fontWeight: 600 }}>{siteConfig.name}</div>
        </div>
        <div style={{ marginTop: 48, fontSize: 56, fontWeight: 600, lineHeight: 1.2, maxWidth: 900 }}>
          {siteConfig.tagline}
        </div>
        <div style={{ marginTop: 24, fontSize: 26, color: "#94a3b8" }}>{siteConfig.company}</div>
      </div>
    ),
    { ...size }
  );
}
