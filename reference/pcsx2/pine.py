"""PINE client for PCSX2 (IPC over a Unix socket): EE memory reads and writes, savestates, status."""
import os
import socket
import struct
import time

READ8, READ16, READ32, READ64 = 0, 1, 2, 3
WRITE8, WRITE16, WRITE32, WRITE64 = 4, 5, 6, 7
VERSION, SAVE_STATE, LOAD_STATE, TITLE, ID, UUID, GAME_VERSION, STATUS = 8, 9, 10, 11, 12, 13, 14, 15
DEFAULT_SLOT = 28011


def socket_path(slot):
    base = os.environ.get('XDG_RUNTIME_DIR') or '/tmp'
    path = os.path.join(base, 'pcsx2.sock')
    return path if slot == DEFAULT_SLOT else f'{path}.{slot}'


class Pine:
    def __init__(self, slot, timeout=30.0):
        self.path = socket_path(slot)
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.connect(self.path)
                break
            except OSError:
                self.sock.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.2)

    def close(self):
        self.sock.close()

    def _call(self, op, args=b''):
        self.sock.sendall(struct.pack('<IB', 5 + len(args), op) + args)
        head = self._recv(4)
        size = struct.unpack('<I', head)[0]
        body = self._recv(size - 4)
        if body[0] != 0:
            raise RuntimeError(f'PINE op {op} failed')
        return body[1:]

    def _recv(self, n):
        out = b''
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise ConnectionError('PINE socket closed')
            out += chunk
        return out

    def read32(self, address):
        return struct.unpack('<I', self._call(READ32, struct.pack('<I', address)))[0]

    def read8(self, address):
        return self._call(READ8, struct.pack('<I', address))[0]

    def read_block(self, address, size):
        return b''.join(struct.pack('<Q', struct.unpack('<Q', self._call(READ64, struct.pack('<I', address + i)))[0])
                        for i in range(0, size, 8))[:size]

    def write32(self, address, value):
        self._call(WRITE32, struct.pack('<II', address, value))

    def status(self):
        return struct.unpack('<I', self._call(STATUS))[0]

    def game_id(self):
        data = self._call(ID)
        return data[4:4 + struct.unpack('<I', data[:4])[0]].rstrip(b'\0').decode()

    def save_state(self, slot):
        self._call(SAVE_STATE, bytes([slot]))

    def load_state(self, slot):
        self._call(LOAD_STATE, bytes([slot]))
