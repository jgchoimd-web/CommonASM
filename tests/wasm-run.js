// Runs a CommonASM wasm module and prints what it printed.
//
// The module imports exactly two things, fd_write and proc_exit, so they are
// supplied here instead of installing a WASI runtime: node is already
// wherever this is being tested, and a real runtime would add an install step
// to CI for thirty lines of work.

const fs = require('fs');

const modulePath = process.argv[2];
if (!modulePath) {
  console.error('usage: node wasm-run.js MODULE.wasm');
  process.exit(2);
}

const EXITED = Symbol('proc_exit');
const chunks = [];
let memory = null;

function bytesAt(pointer, length) {
  // Copied rather than viewed, since a later memory.grow would detach it.
  return Buffer.from(new Uint8Array(memory.buffer, pointer, length));
}

const wasi_snapshot_preview1 = {
  // fd_write(fd, iovs, iovs_len, nwritten) writes the vector of (pointer,
  // length) pairs at iovs and reports how many bytes went out.
  fd_write(fd, iovs, iovsLen, nwrittenPointer) {
    const view = new DataView(memory.buffer);
    let written = 0;
    for (let i = 0; i < iovsLen; i++) {
      const pointer = view.getUint32(iovs + i * 8, true);
      const length = view.getUint32(iovs + i * 8 + 4, true);
      const bytes = bytesAt(pointer, length);
      if (fd === 2) process.stderr.write(bytes);
      else chunks.push(bytes);
      written += length;
    }
    view.setUint32(nwrittenPointer, written, true);
    return 0;
  },
  proc_exit(code) {
    const stop = new Error('proc_exit');
    stop[EXITED] = code;
    throw stop;
  },
};

WebAssembly.instantiate(fs.readFileSync(modulePath), { wasi_snapshot_preview1 })
  .then(({ instance }) => {
    memory = instance.exports.memory;
    let status = 0;
    try {
      instance.exports._start();
    } catch (error) {
      if (error && error[EXITED] !== undefined) status = error[EXITED];
      else throw error;
    }
    process.stdout.write(Buffer.concat(chunks));
    process.exit(status);
  })
  .catch((error) => {
    console.error(error && error.message ? error.message : error);
    process.exit(1);
  });
