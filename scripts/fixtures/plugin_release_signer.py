"""Disposable test Ed25519 signer. Its private key stays in pipes and memory."""
import base64
from contextlib import contextmanager
import hashlib
from pathlib import Path
import socket
import subprocess
import sys
import threading


MAX_SIGN_INPUT = 65564


def sha(data):
    return hashlib.sha256(data).hexdigest()


@contextmanager
def memory_signer(work):
    def openssl(*args, data=None):
        return subprocess.run(['openssl', *args], input=data, capture_output=True,
                              timeout=5, check=True).stdout

    key = openssl('genpkey', '-algorithm', 'Ed25519')
    der = openssl('pkey', '-pubout', '-outform', 'DER', data=key)
    assert len(der) == 44 and der[:12] == bytes.fromhex('302a300506032b6570032100')
    endpoint = work / 's'
    errors = []

    stop = threading.Event()

    def serve():
        try:
            while not stop.is_set():
                try:
                    connection, _ = server.accept()
                except TimeoutError:
                    continue
                with connection:
                    connection.settimeout(5)
                    with connection.makefile('rb') as stream:
                        message = stream.read(MAX_SIGN_INPUT + 1)
                    if not 1 <= len(message) <= MAX_SIGN_INPUT:
                        raise RuntimeError('Invalid signing input size')
                    path = work / 'signing-input'
                    path.write_bytes(message)
                    signature = openssl('pkeyutl', '-sign', '-rawin', '-inkey', '/dev/stdin',
                                        '-in', str(path), data=key)
                    assert len(signature) == 64
                    connection.sendall(signature)
        except Exception as error:
            errors.append(error)

    with socket.socket(socket.AF_UNIX) as server:
        server.settimeout(0.1)
        server.bind(str(endpoint))
        endpoint.chmod(0o600)
        server.listen(1)
        worker = threading.Thread(target=serve)
        worker.start()
        try:
            yield base64.b64encode(der[12:]).decode('ascii'), endpoint
        finally:
            stop.set()
            worker.join()
        if errors:
            raise RuntimeError('Temporary signer failed') from errors[0]


def sign(endpoint, data):
    if not 1 <= len(data) <= MAX_SIGN_INPUT:
        raise SystemExit('Invalid signing input size')
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(endpoint))
        connection.sendall(data)
        connection.shutdown(socket.SHUT_WR)
        with connection.makefile('rb') as stream:
            signature = stream.read(65)
        if len(signature) != 64:
            raise SystemExit('Invalid signature size')
        return signature


def signer_config(endpoint, marker=None):
    executable = Path(sys.executable).resolve(strict=True)
    arguments = ['-IB', str(Path(__file__).resolve()), str(endpoint)]
    if marker is not None:
        arguments.append(str(marker))
    return dict(executable=str(executable), sha256=sha(executable.read_bytes()),
                prefixArguments=arguments)


if __name__ == '__main__':
    if len(sys.argv) == 3:
        Path(sys.argv[2]).touch()
    sys.stdout.buffer.write(sign(sys.argv[1], sys.stdin.buffer.read(MAX_SIGN_INPUT + 1)))
