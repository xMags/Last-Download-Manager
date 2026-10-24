// Builds the browser extension for each browser family (M3 design section 2): copies the shared
// sources and that family's manifest into <out>/<family>/, with the version taken from the
// project version in CMakeLists.txt so LDM and its extension always match.
//
// Usage: node extension/build.mjs [--out DIR]   (default: build/extension)
// Uses only Node's standard library.

import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const source = join(root, 'extension');
const families = ['chromium', 'firefox'];
// What ships; tests and manifests for other families stay out.
const shipped = ['src', 'pages', 'icons'];

function outputFolder() {
  const index = process.argv.indexOf('--out');
  if (index === -1) {
    return join(root, 'build', 'extension');
  }
  const value = process.argv[index + 1];
  if (!value) {
    throw new Error('--out needs a folder');
  }
  return resolve(value);
}

function projectVersion() {
  const cmake = readFileSync(join(root, 'CMakeLists.txt'), 'utf8');
  const match = /project\(LastDownloadManager\s+VERSION\s+(\d+\.\d+\.\d+)/.exec(cmake);
  if (!match) {
    throw new Error('Could not find the project version in CMakeLists.txt');
  }
  return match[1];
}

const out = outputFolder();
const version = projectVersion();
for (const family of families) {
  const target = join(out, family);
  rmSync(target, { recursive: true, force: true });
  mkdirSync(target, { recursive: true });
  for (const folder of shipped) {
    cpSync(join(source, folder), join(target, folder), { recursive: true });
  }
  const manifest = JSON.parse(readFileSync(join(source, `manifest.${family}.json`), 'utf8'));
  manifest.version = version;
  writeFileSync(join(target, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);
  console.log(`${family}: ${target}`);
}
