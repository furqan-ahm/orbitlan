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
  .resize(470, 470, { fit: "contain", kernel: sharp.kernel.nearest })
  .png()
  .toBuffer();

const socialBackground = Buffer.from(`
<svg width="1200" height="630" viewBox="0 0 1200 630" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="sky" x1="0" y1="0" x2="0.92" y2="1">
      <stop offset="0" stop-color="#090e43"/>
      <stop offset="0.56" stop-color="#111755"/>
      <stop offset="1" stop-color="#40316b"/>
    </linearGradient>
    <radialGradient id="glow" cx="50%" cy="50%" r="50%">
      <stop offset="0" stop-color="#6f7ce8" stop-opacity="0.23"/>
      <stop offset="1" stop-color="#6f7ce8" stop-opacity="0"/>
    </radialGradient>
  </defs>
  <rect width="1200" height="630" fill="url(#sky)"/>
  <circle cx="955" cy="315" r="300" fill="url(#glow)"/>
  <g fill="#e3e5ff">
    <rect x="66" y="74" width="4" height="4"/><rect x="211" y="119" width="3" height="3"/>
    <rect x="356" y="67" width="4" height="4"/><rect x="538" y="101" width="3" height="3"/>
    <rect x="681" y="56" width="4" height="4"/><rect x="1103" y="91" width="3" height="3"/>
    <rect x="92" y="528" width="3" height="3"/><rect x="444" y="552" width="4" height="4"/>
    <rect x="660" y="504" width="3" height="3"/><rect x="1138" y="512" width="4" height="4"/>
  </g>
  <text x="80" y="140" fill="#c9c7ff" font-family="DejaVu Sans Mono, monospace" font-size="18" font-weight="700" letter-spacing="1.5">FREE + OPEN SOURCE</text>
  <text x="76" y="244" fill="#ffffff" font-family="DejaVu Sans, Arial, sans-serif" font-size="86" font-weight="700" letter-spacing="-4">OrbitLan</text>
  <text x="80" y="310" fill="#e2e3ff" font-family="DejaVu Sans, Arial, sans-serif" font-size="34" font-weight="700">Virtual LAN for Windows + Linux</text>
  <text x="80" y="375" fill="#bcc0e5" font-family="DejaVu Sans, Arial, sans-serif" font-size="23">Create a room. Share the code. Play.</text>
  <line x1="80" y1="451" x2="560" y2="451" stroke="#8386b8" stroke-width="1"/>
  <text x="80" y="497" fill="#ffffff" font-family="DejaVu Sans, Arial, sans-serif" font-size="21" font-weight="700">orbitlan.site</text>
  <text x="80" y="532" fill="#aeb2d9" font-family="DejaVu Sans, Arial, sans-serif" font-size="17">Games · servers · private IP tools</text>
</svg>`);

await sharp(socialBackground)
  .composite([{ input: earth, left: 700, top: 80 }])
  .png({ compressionLevel: 9 })
  .toFile(path.join(assetsDir, "orbitlan-social.png"));

console.log("Generated favicon, app icons, and social preview.");
