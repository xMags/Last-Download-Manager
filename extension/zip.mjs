// A small zip writer for the store packages (M5 design section 4): deflated entries, no
// encryption, no zip64. Uses only Node's standard library.

import { crc32, deflateRawSync } from 'node:zlib';

// DOS date and time for every entry: a fixed moment, so the same files give the same zip.
const DOS_TIME = 0;
const DOS_DATE = ((2026 - 1980) << 9) | (1 << 5) | 1;

/**
 * The bytes of a zip archive holding `files`.
 * @param {{name: string, data: Buffer}[]} files names use '/' and are relative
 * @returns {Buffer}
 */
export function zip(files) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const file of files) {
    if (file.name.startsWith('/') || file.name.includes('\\') || file.name.split('/').includes('..')) {
      throw new Error(`not a relative entry name: ${file.name}`);
    }
    const name = Buffer.from(file.name, 'utf8');
    const compressed = deflateRawSync(file.data, { level: 9 });
    const checksum = crc32(file.data);

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);          // version needed
    local.writeUInt16LE(0x0800, 6);      // UTF-8 names
    local.writeUInt16LE(8, 8);           // deflate
    local.writeUInt16LE(DOS_TIME, 10);
    local.writeUInt16LE(DOS_DATE, 12);
    local.writeUInt32LE(checksum, 14);
    local.writeUInt32LE(compressed.length, 18);
    local.writeUInt32LE(file.data.length, 22);
    local.writeUInt16LE(name.length, 26);
    local.writeUInt16LE(0, 28);
    locals.push(local, name, compressed);

    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0);
    central.writeUInt16LE(20, 4);        // made by
    central.writeUInt16LE(20, 6);        // version needed
    central.writeUInt16LE(0x0800, 8);
    central.writeUInt16LE(8, 10);
    central.writeUInt16LE(DOS_TIME, 12);
    central.writeUInt16LE(DOS_DATE, 14);
    central.writeUInt32LE(checksum, 16);
    central.writeUInt32LE(compressed.length, 20);
    central.writeUInt32LE(file.data.length, 24);
    central.writeUInt16LE(name.length, 28);
    central.writeUInt32LE(offset, 42);
    centrals.push(central, name);

    offset += local.length + name.length + compressed.length;
  }
  const directory = Buffer.concat(centrals);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(files.length, 8);
  end.writeUInt16LE(files.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, directory, end]);
}
