import fs from "node:fs/promises";
import path from "node:path";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const sharp = require("../coordinator/node_modules/sharp");

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const repoDir = path.resolve(scriptDir, "..");
const websiteDir = path.join(repoDir, "website");
const assetsDir = path.join(websiteDir, "assets");
const earthSource = path.join(assetsDir, "earthspin-transparent.webp");
const cloudyEarthSource = path.join(assetsDir, "earth-clouds.gif");

const iconSizes = [48, 192, 512];
for (const size of iconSizes) {
  await sharp(earthSource, { page: 0 })
    .resize(size, size, { fit: "contain", kernel: sharp.kernel.nearest })
    .png({ compressionLevel: 9, palette: true })
    .toFile(path.join(assetsDir, `orbitlan-icon-${size}.png`));
}

await sharp(earthSource, { page: 0 })
  .resize(180, 180, { fit: "contain", kernel: sharp.kernel.nearest })
  .png({ compressionLevel: 9, palette: true })
  .toFile(path.join(websiteDir, "apple-touch-icon.png"));

const faviconPng = await fs.readFile(path.join(assetsDir, "orbitlan-icon-48.png"));
const icoHeader = Buffer.alloc(22);
icoHeader.writeUInt16LE(0, 0);
icoHeader.writeUInt16LE(1, 2);
icoHeader.writeUInt16LE(1, 4);
icoHeader.writeUInt8(48, 6);
icoHeader.writeUInt8(48, 7);
icoHeader.writeUInt8(0, 8);
icoHeader.writeUInt8(0, 9);
icoHeader.writeUInt16LE(1, 10);
icoHeader.writeUInt16LE(32, 12);
icoHeader.writeUInt32LE(faviconPng.length, 14);
icoHeader.writeUInt32LE(22, 18);
await fs.writeFile(path.join(websiteDir, "favicon.ico"), Buffer.concat([icoHeader, faviconPng]));

const earth = await sharp(cloudyEarthSource, { page: 0 })
  .resize(430, 430, { fit: "contain", kernel: sharp.kernel.nearest })
  .png()
  .toBuffer();

const socialBackground = Buffer.from(`
<svg width="1200" height="630" viewBox="0 0 1200 630" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#080d3e"/>
      <stop offset="0.58" stop-color="#171957"/>
      <stop offset="1" stop-color="#49356f"/>
    </linearGradient>
    <radialGradient id="glow" cx="50%" cy="50%" r="50%">
      <stop offset="0" stop-color="#6379e8" stop-opacity="0.34"/>
      <stop offset="1" stop-color="#6379e8" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <rect width="1200" height="630" fill="url(#sky)"/>
  <circle cx="940" cy="315" r="290" fill="url(#glow)"/>
  <g fill="#d9dcff" opacity="0.88">
    <rect x="68" y="76" width="4" height="4"/><rect x="206" y="116" width="3" height="3"/>
    <rect x="355" y="65" width="4" height="4"/><rect x="536" y="102" width="3" height="3"/>
    <rect x="684" y="58" width="4" height="4"/><rect x="1101" y="91" width="3" height="3"/>
    <rect x="93" y="523" width="3" height="3"/><rect x="444" y="554" width="4" height="4"/>
    <rect x="662" y="498" width="3" height="3"/><rect x="1138" y="506" width="4" height="4"/>
  </g>
  <g fill="none" stroke="#9da8ff" stroke-width="2" opacity="0.48">
    <ellipse cx="945" cy="315" rx="270" ry="112" transform="rotate(-8 945 315)"/>
    <ellipse cx="945" cy="315" rx="248" ry="178" transform="rotate(18 945 315)"/>
  </g>
  <g fill="#9ae4bd">
    <circle cx="706" cy="273" r="8"/><circle cx="1088" cy="202" r="8"/>
    <circle cx="1146" cy="366" r="8"/><circle cx="818" cy="480" r="8"/>
  </g>
  <text x="76" y="235" fill="#ffffff" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="82" font-weight="800" letter-spacing="-4">OrbitLan</text>
  <text x="80" y="306" fill="#dfe1ff" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="34" font-weight="650">Your private LAN, online.</text>
  <text x="80" y="363" fill="#bbc0e9" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="23">Open source virtual LAN for games,</text>
  <text x="80" y="397" fill="#bbc0e9" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="23">servers, Windows and Linux.</text>
  <rect x="80" y="452" width="225" height="52" rx="10" fill="#ffffff"/>
  <text x="192.5" y="486" fill="#171743" text-anchor="middle" font-family="Segoe UI, Inter, Arial, sans-serif" font-size="18" font-weight="750">FREE &amp; OPEN SOURCE</text>
</svg>`);

await sharp(socialBackground)
  .composite([{ input: earth, left: 730, top: 100 }])
  .png({ compressionLevel: 9 })
  .toFile(path.join(assetsDir, "orbitlan-social.png"));

console.log("Generated favicon, app icons, and social preview.");
