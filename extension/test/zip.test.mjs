import assert from 'node:assert/strict';
import { test } from 'node:test';
import { crc32, inflateRawSync } from 'node:zlib';

import { zip } from '../zip.mjs';

// Reads the entries back through the central directory, as a zip reader would.
function entries(archive) {
  const end = archive.length - 22;
  assert.equal(archive.readUInt32LE(end), 0x06054b50);
  const count = archive.readUInt16LE(end + 10);
  let position = archive.readUInt32LE(end + 16);
  const found = [];
  for (let i = 0; i < count; ++i) {
    assert.equal(archive.readUInt32LE(position), 0x02014b50);
    const compressedSize = archive.readUInt32LE(position + 20);
    const size = archive.readUInt32LE(position + 24);
    const checksum = archive.readUInt32LE(position + 16);
    const nameLength = archive.readUInt16LE(position + 28);
    const local = archive.readUInt32LE(position + 42);
    const name = archive.subarray(position + 46, position + 46 + nameLength).toString('utf8');
    assert.equal(archive.readUInt32LE(local), 0x04034b50);
    const localNameLength = archive.readUInt16LE(local + 26);
    const dataStart = local + 30 + localNameLength;
    const data = inflateRawSync(archive.subarray(dataStart, dataStart + compressedSize));
    assert.equal(data.length, size);
    assert.equal(crc32(data), checksum);
    found.push({ name, data: data.toString('utf8') });
    position += 46 + nameLength;
  }
  return found;
}

test('zip writes entries a reader gets back unchanged', () => {
  const files = [
    { name: 'manifest.json', data: Buffer.from('{"version":"5.0.0"}\n') },
    { name: 'src/background.js', data: Buffer.from('console.log("é");\n'.repeat(50)) },
    { name: 'empty.txt', data: Buffer.alloc(0) },
  ];
  const archive = zip(files);
  assert.deepEqual(entries(archive), files.map((file) => ({ name: file.name, data: file.data.toString('utf8') })));
  assert.deepEqual(zip(files), archive, 'the same files give the same bytes');
});

test('zip refuses names that leave the archive', () => {
  for (const name of ['/etc/passwd', '../up.js', 'a/../../b', 'a\\b']) {
    assert.throws(() => zip([{ name, data: Buffer.from('x') }]), /relative/, name);
  }
});
