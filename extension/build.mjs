// Builds the browser extension for each browser family (M3 design section 2): copies the shared
// sources and that family's manifest into <out>/<family>/, with the version taken from the
// project version in CMakeLists.txt so LDM and its extension always match.
//
// Usage: node extension/build.mjs [--out DIR] [--zip]   (default: build/extension)
// --zip also writes <out>/LastDownloadManager-<family>.zip, the packages for the browser stores
// (M5 design section 4). The Chromium package leaves out the manifest's "key", which only pins
// the id of the unpacked development build; the stores assign their own.
// Uses only Node's standard library.

import { cpSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

import { zip } from './zip.mjs';

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

// Every file below `folder`, by its '/'-separated path, in a stable order.
function filesBelow(folder) {
  const files = [];
  const walk = (current) => {
    for (const name of readdirSync(current).sort()) {
      const path = join(current, name);
      if (statSync(path).isDirectory()) {
        walk(path);
      } else {
        files.push({ name: relative(folder, path).split(sep).join('/'), path });
      }
    }
  };
  walk(folder);
  return files;
}

function writeStorePackage(family, target, manifest) {
  const store = { ...manifest };
  delete store.key;
  const files = filesBelow(target).map(({ name, path }) => ({
    name,
    data: name === 'manifest.json' ? Buffer.from(`${JSON.stringify(store, null, 2)}\n`) : readFileSync(path),
  }));
  const file = join(out, `LastDownloadManager-${family}.zip`);
  writeFileSync(file, zip(files));
  console.log(`${family} store package: ${file}`);
}

const out = outputFolder();
const version = projectVersion();
const zipPackages = process.argv.includes('--zip');
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
  if (zipPackages) {
    writeStorePackage(family, target, manifest);
  }
}
