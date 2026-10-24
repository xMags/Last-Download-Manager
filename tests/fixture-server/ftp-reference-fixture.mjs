// FTP side of the engine test server (checklist ENG-13). Serves the same deterministic payload as
// the HTTP fixture and records every transfer in the same log. Never serves files from the host.
//
// Paths:
//   /range.bin           the payload; SIZE, MDTM and REST supported
//   anything else        550
// Logins: "anonymous" (any password) and "ldm"/"secret" use REST normally. The login "norest"
// (any password) answers REST with 502, like servers that cannot resume.
// Data connections: passive (PASV, EPSV) and active (PORT, EPRT), on 127.0.0.1 only.
import { createServer, connect } from 'node:net';

const PATH = '/range.bin';

export function createFtpFixture({ data, delayMs = 40, chunkSize = 32 * 1024, record = () => {}, nextId = () => 0 }) {
  let active = 0;

  const server = createServer(control => {
    control.setEncoding('latin1');
    let user = '';
    let loggedIn = false;
    let restOffset = 0;
    let passive = null;         // net.Server waiting for the client's data connection
    let passiveSocket = null;   // accepted data connection
    let activeTarget = null;    // { host, port } from PORT/EPRT
    let transfer = null;        // the running RETR
    let buffered = '';

    const reply = text => { if (!control.destroyed) control.write(`${text}\r\n`); };
    const closePassive = () => {
      if (passive) { passive.close(); passive = null; }
    };

    const openPassive = callback => {
      closePassive();
      passiveSocket = null;
      passive = createServer(socket => {
        passiveSocket = socket;
        closePassive();
      });
      passive.listen(0, '127.0.0.1', () => callback(passive.address().port));
    };

    // Resolves with the data socket once the client is connected (passive) or reached (active).
    const dataSocket = () => new Promise((resolve, reject) => {
      if (activeTarget) {
        const socket = connect(activeTarget.port, activeTarget.host);
        socket.once('connect', () => resolve(socket));
        socket.once('error', reject);
        activeTarget = null;
        return;
      }
      if (passiveSocket) { resolve(passiveSocket); passiveSocket = null; return; }
      if (!passive) { reject(new Error('no data connection')); return; }
      passive.once('connection', socket => { passiveSocket = null; resolve(socket); });
      setTimeout(() => reject(new Error('data connection timeout')), 10000).unref();
    });

    const retrieve = async path => {
      if (path !== PATH && path !== PATH.slice(1)) { reply('550 No such file.'); return; }
      const start = restOffset;
      restOffset = 0;
      if (start > data.length) { reply('554 Invalid REST parameter.'); return; }
      let socket;
      try {
        socket = await dataSocket();
      } catch {
        reply('425 Cannot open data connection.');
        return;
      }
      const id = nextId();
      active += 1;
      record({ event: 'ftp-retr', id, user, start, active });
      reply(`150 Opening BINARY mode data connection for range.bin (${data.length - start} bytes).`);
      let offset = start;
      let timer;
      let aborted = false;
      transfer = { abort: () => { aborted = true; clearTimeout(timer); socket.destroy(); } };
      let done = false;
      const finish = completed => {
        if (done) return;
        done = true;
        clearTimeout(timer);
        active -= 1;
        transfer = null;
        record({ event: 'ftp-closed', id, start, bytesWritten: offset - start, finished: completed });
        reply(completed ? '226 Transfer complete.' : '426 Connection closed; transfer aborted.');
      };
      socket.on('error', () => {});
      // A client that knows the size (from SIZE) may close the data connection itself after the
      // last byte; that is a complete transfer, not an abort.
      socket.once('close', () => finish(!aborted && offset >= data.length));
      const send = () => {
        if (socket.destroyed) return;
        if (offset >= data.length) {
          socket.end(() => finish(true));
          return;
        }
        const next = Math.min(offset + chunkSize, data.length);
        const ready = socket.write(data.subarray(offset, next));
        offset = next;
        const schedule = () => { timer = setTimeout(send, delayMs); };
        if (ready) schedule();
        else socket.once('drain', schedule);
      };
      send();
    };

    const handle = line => {
      const space = line.indexOf(' ');
      const command = (space < 0 ? line : line.slice(0, space)).toUpperCase();
      const argument = space < 0 ? '' : line.slice(space + 1);
      record({ event: 'ftp-command', command, argument: command === 'PASS' ? '***' : argument });
      if (!loggedIn && !['USER', 'PASS', 'QUIT', 'FEAT', 'SYST', 'OPTS', 'AUTH'].includes(command)) {
        reply('530 Please log in.');
        return;
      }
      switch (command) {
        case 'USER':
          user = argument;
          reply('331 Password required.');
          return;
        case 'PASS':
          if (user === 'anonymous' || user === 'norest' || (user === 'ldm' && argument === 'secret')) {
            loggedIn = true;
            reply('230 Logged in.');
          } else {
            reply('530 Login incorrect.');
          }
          return;
        case 'AUTH':
          reply('502 TLS not available.');
          return;
        case 'SYST':
          reply('215 UNIX Type: L8');
          return;
        case 'FEAT':
          control.write('211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n EPSV\r\n UTF8\r\n211 End\r\n');
          return;
        case 'OPTS':
          reply('200 OK.');
          return;
        case 'PWD':
          reply('257 "/" is the current directory.');
          return;
        case 'CWD':
          reply('250 OK.');
          return;
        case 'TYPE':
          reply('200 Type set.');
          return;
        case 'NOOP':
          reply('200 OK.');
          return;
        case 'SIZE':
          if (argument === PATH || argument === PATH.slice(1)) reply(`213 ${data.length}`);
          else reply('550 No such file.');
          return;
        case 'MDTM':
          if (argument === PATH || argument === PATH.slice(1)) reply('213 20260101120000');
          else reply('550 No such file.');
          return;
        case 'REST': {
          if (user === 'norest') { reply('502 REST not implemented.'); return; }
          const value = Number(argument);
          if (!Number.isSafeInteger(value) || value < 0) { reply('501 Invalid REST parameter.'); return; }
          restOffset = value;
          reply(`350 Restarting at ${value}.`);
          return;
        }
        case 'PASV':
          openPassive(port => reply(`227 Entering Passive Mode (127,0,0,1,${port >> 8},${port & 255}).`));
          return;
        case 'EPSV':
          openPassive(port => reply(`229 Entering Extended Passive Mode (|||${port}|).`));
          return;
        case 'PORT': {
          const parts = argument.split(',').map(Number);
          if (parts.length !== 6 || parts.some(part => !Number.isInteger(part) || part < 0 || part > 255) ||
              parts.slice(0, 4).join('.') !== '127.0.0.1') {
            reply('501 Only 127.0.0.1 is allowed.');
            return;
          }
          activeTarget = { host: '127.0.0.1', port: parts[4] * 256 + parts[5] };
          reply('200 PORT command successful.');
          return;
        }
        case 'EPRT': {
          const fields = argument.split(argument[0] ?? '|');
          const port = Number(fields[3]);
          if (fields[2] !== '127.0.0.1' || !Number.isInteger(port) || port < 1 || port > 65535) {
            reply('501 Only 127.0.0.1 is allowed.');
            return;
          }
          activeTarget = { host: '127.0.0.1', port };
          reply('200 EPRT command successful.');
          return;
        }
        case 'RETR':
          retrieve(argument);
          return;
        case 'ABOR':
          if (transfer) transfer.abort();
          reply('226 Abort successful.');
          return;
        case 'QUIT':
          reply('221 Goodbye.');
          control.end();
          return;
        default:
          reply('502 Command not implemented.');
      }
    };

    control.on('data', chunk => {
      buffered += chunk;
      if (buffered.length > 4096) { control.destroy(); return; }
      let newline;
      while ((newline = buffered.indexOf('\n')) >= 0) {
        const line = buffered.slice(0, newline).replace(/\r$/, '');
        buffered = buffered.slice(newline + 1);
        handle(line);
      }
    });
    control.on('error', () => {});
    control.on('close', () => {
      closePassive();
      if (transfer) transfer.abort();
    });
    reply('220 LDM FTP fixture ready.');
  });

  return { server, activeTransfers: () => active };
}
