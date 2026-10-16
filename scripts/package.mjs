#!/usr/bin/env node
// Builds the release ZIP and FrameDrop manifest. Never runs the binary.
// Adapted from KominoVR/frame-passthrough-shortcuts (MIT).
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { deflateRawSync } from 'node:zlib';
import { fileURLToPath, pathToFileURL } from 'node:url';

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const appName = 'knockthru';
const controlName = 'knockthru-ctl';
const displayName = 'KnockThru';

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// Small ZIP32 writer with explicit Unix modes, so Linux executable bits survive
// cross-platform packaging. All file names are relative, UTF-8, and use '/'.
export function createZip(entries) {
  if (entries.length > 65535) throw new Error('Too many ZIP entries');
  const local = [], central = [];
  let offset = 0;
  for (const entry of entries) {
    if (entry.name.startsWith('/') || entry.name.includes('\\') || entry.name.split('/').includes('..')) {
      throw new Error(`Unsafe ZIP path: ${entry.name}`);
    }
    const name = Buffer.from(entry.name, 'utf8');
    const data = Buffer.from(entry.data);
    const compressed = deflateRawSync(data, { level: 9 });
    if (data.length > 0xffffffff || offset > 0xffffffff) throw new Error('ZIP64 is not supported');
    const checksum = crc32(data);
    const header = Buffer.alloc(30);
    header.writeUInt32LE(0x04034b50, 0);
    header.writeUInt16LE(20, 4);
    header.writeUInt16LE(0x800, 6);
    header.writeUInt16LE(8, 8);
    header.writeUInt16LE(33, 12); // 1980-01-01, for reproducible archives.
    header.writeUInt32LE(checksum, 14);
    header.writeUInt32LE(compressed.length, 18);
    header.writeUInt32LE(data.length, 22);
    header.writeUInt16LE(name.length, 26);
    local.push(header, name, compressed);

    const directory = Buffer.alloc(46);
    directory.writeUInt32LE(0x02014b50, 0);
    directory.writeUInt16LE(0x314, 4); // Unix, ZIP 2.0.
    directory.writeUInt16LE(20, 6);
    directory.writeUInt16LE(0x800, 8);
    directory.writeUInt16LE(8, 10);
    directory.writeUInt16LE(33, 14);
    directory.writeUInt32LE(checksum, 16);
    directory.writeUInt32LE(compressed.length, 20);
    directory.writeUInt32LE(data.length, 24);
    directory.writeUInt16LE(name.length, 28);
    directory.writeUInt32LE(((0o100000 | entry.mode) << 16) >>> 0, 38);
    directory.writeUInt32LE(offset, 42);
    central.push(directory, name);
    offset += header.length + name.length + compressed.length;
  }
  const directory = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...local, directory, end]);
}

export function verifyBinary(bytes, platform) {
  if (platform === 'linux-arm64') {
    if (bytes.length < 64 || bytes.subarray(0, 4).toString('hex') !== '7f454c46' ||
        bytes[4] !== 2 || bytes[5] !== 1 || bytes.readUInt16LE(18) !== 183) {
      throw new Error('linux-arm64 requires a 64-bit little-endian AArch64 ELF executable');
    }
    if (![2, 3].includes(bytes.readUInt16LE(16))) throw new Error('ELF file is not executable');
  } else throw new Error(`Unknown platform: ${platform}`);
}

export function publicBaseUrl(value) {
  const url = new URL(value);
  if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash) {
    throw new Error('--base-url must be an HTTPS directory URL without credentials, a query, or a fragment');
  }
  // FrameDrop disallows LAN addresses and credentials. Reject literal IPs and
  // local host names rather than emitting a link its public installer rejects.
  if (url.hostname === 'localhost' || url.hostname.endsWith('.localhost') ||
      !url.hostname.includes('.') || /^\d+(\.\d+){3}$/.test(url.hostname) || url.hostname.includes(':') ||
      url.hostname.endsWith('.local')) throw new Error('--base-url must use a public host name');
  url.pathname = url.pathname.replace(/\/?$/, '/');
  return url;
}

function collectFiles(directory, prefix, entries) {
  for (const item of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    if (item.isSymbolicLink()) throw new Error(`Symlinks are not allowed in the bundle: ${item.name}`);
    const source = path.join(directory, item.name);
    const name = prefix ? `${prefix}/${item.name}` : item.name;
    if (item.isDirectory()) collectFiles(source, name, entries);
    else if (item.isFile()) entries.push({ name, data: fs.readFileSync(source), mode: 0o644 });
  }
}

export function packageRelease(options) {
  const root = path.resolve(options.root ?? projectRoot);
  const version = options.version ?? '1.0.0';
  if (!/^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/.test(version)) throw new Error('Version must be semver, e.g. 1.0.0');
  const platform = options.platform ?? 'linux-arm64';
  const binaryName = appName;
  const binaryPath = path.resolve(options.buildDir ?? path.join(root, 'build-arm64'), 'bin', binaryName);
  const binary = fs.readFileSync(binaryPath);
  verifyBinary(binary, platform);
  const entries = [{ name: binaryName, data: binary, mode: 0o755 }];
  if (platform === 'linux-arm64') {
    const controlPath = path.join(path.dirname(binaryPath), controlName);
    if (!fs.existsSync(controlPath) || !fs.lstatSync(controlPath).isFile()) {
      throw new Error(`Linux release requires a regular management companion: ${controlName}`);
    }
    const control = fs.readFileSync(controlPath);
    verifyBinary(control, platform);
    if (!control.equals(binary)) {
      throw new Error(`${controlName} must match the main executable exactly; rebuild with scripts/build-arm64.sh`);
    }
    entries.push({ name: controlName, data: control, mode: 0o755 });
  }
  for (const filename of ['config.json', 'actions.json']) {
    JSON.parse(fs.readFileSync(path.join(root, 'assets', filename), 'utf8'));
  }
  collectFiles(path.join(root, 'assets'), '', entries);
  for (const filename of ['README.md', 'LICENSE', 'third_party/openvr/LICENSE', 'third_party/json/LICENSE.MIT', 'third_party/vulkan/MIT.txt']) {
    entries.push({ name: filename, data: fs.readFileSync(path.join(root, filename)), mode: 0o644 });
  }
  for (const filename of ['THIRD_PARTY_NOTICES.md', 'images/framedrop-button.svg']) {
    const source = path.join(root, filename);
    if (fs.existsSync(source)) entries.push({ name: filename, data: fs.readFileSync(source), mode: 0o644 });
  }
  const compilerNotices = path.join(root, 'third_party/toolchain-licenses');
  if (fs.existsSync(compilerNotices)) collectFiles(compilerNotices, 'third_party/toolchain-licenses', entries);
  const names = new Set();
  for (const entry of entries) {
    if (names.has(entry.name)) throw new Error(`Duplicate archive entry: ${entry.name}`);
    names.add(entry.name);
  }
  const zipName = `${appName}-${version}-${platform}.zip`;
  const outputDir = path.resolve(options.outputDir ?? path.join(root, 'dist'));
  const base = options.baseUrl ? publicBaseUrl(options.baseUrl) : undefined;
  if (options.fileUrl && !base) throw new Error('--file-url requires --base-url');
  const zip = createZip(entries);
  const sha256 = createHash('sha256').update(zip).digest('hex');
  fs.mkdirSync(outputDir, { recursive: true });
  fs.writeFileSync(path.join(outputDir, zipName), zip);
  fs.writeFileSync(path.join(outputDir, `${zipName}.sha256`), `${sha256}  ${zipName}\n`);
  const result = { archive: path.join(outputDir, zipName), sha256 };
  if (base && platform === 'linux-arm64') {
    const manifestName = `${appName}.framedrop.json`;
    const fileUrl = options.fileUrl ? new URL(options.fileUrl) : new URL(zipName, base);
    if (fileUrl.protocol !== 'https:' || fileUrl.username || fileUrl.password || fileUrl.search || fileUrl.hash ||
        !fileUrl.pathname.endsWith(`/${zipName}`)) {
      throw new Error('--file-url must be an HTTPS URL ending in the release ZIP filename');
    }
    const manifest = { schema: 'framedrop.install/v1', name: displayName, files: [{ url: fileUrl.href, sha256 }] };
    fs.writeFileSync(path.join(outputDir, manifestName), JSON.stringify(manifest, null, 2) + '\n');
    const manifestUrl = options.installManifestUrl ?? latestManifestUrl(base, manifestName);
    const installUrl = 'https://framedropvr.com/install?manifest=' + encodeURIComponent(manifestUrl);
    result.manifest = path.join(outputDir, manifestName);
    result.installUrl = installUrl;
  }
  return result;
}

function latestManifestUrl(base, manifestName) {
  const match = base.pathname.match(/^\/([^/]+)\/([^/]+)\/releases\/download\/[^/]+\/$/);
  if (base.hostname === 'github.com' && match) {
    return new URL(`/${match[1]}/${match[2]}/releases/latest/download/${manifestName}`, base.origin).href;
  }
  return new URL(manifestName, base).href;
}

function main(args) {
  if (args.includes('--help')) {
    console.log('node scripts/package.mjs --build-dir build-arm64 --version 1.0.0 --platform linux-arm64 [--output-dir dist] [--base-url https://host/releases/v1.0.0/] [--file-url https://host/download/knockthru-1.0.0-linux-arm64.zip] [--install-manifest-url https://host/releases/latest/download/knockthru.framedrop.json]');
    return;
  }
  const mapping = { '--build-dir': 'buildDir', '--version': 'version', '--platform': 'platform', '--output-dir': 'outputDir', '--base-url': 'baseUrl', '--file-url': 'fileUrl', '--install-manifest-url': 'installManifestUrl' };
  const options = {};
  for (let i = 0; i < args.length; i += 2) {
    if (!mapping[args[i]] || !args[i + 1] || args[i + 1].startsWith('--')) throw new Error(`Unknown option or missing value: ${args[i]}`);
    options[mapping[args[i]]] = args[i + 1];
  }
  console.log(JSON.stringify(packageRelease(options), null, 2));
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try { main(process.argv.slice(2)); }
  catch (error) { console.error(`Packaging failed: ${error.message}`); process.exitCode = 1; }
}
