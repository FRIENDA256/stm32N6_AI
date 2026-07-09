#!/usr/bin/env python3
import argparse
import re
import select
import socket
import struct
import threading
import time


BEGIN_MARK = b"BEGIN_STM32N6_THR\r\n"


def mbps(byte_count, seconds):
    if seconds <= 0:
        return 0.0
    return (byte_count * 8.0) / seconds / 1_000_000.0


def recv_until(sock, marker, timeout_s):
    deadline = time.perf_counter() + timeout_s
    data = bytearray()
    while marker not in data:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            raise TimeoutError(f"timed out waiting for {marker!r}")
        ready, _, _ = select.select([sock], [], [], min(0.25, remaining))
        if not ready:
            continue
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("socket closed while waiting for marker")
        data.extend(chunk)
    idx = data.index(marker) + len(marker)
    return bytes(data[:idx]), bytes(data[idx:])


def recv_exact(sock, byte_count, initial=b"", timeout_s=60.0):
    deadline = time.perf_counter() + timeout_s
    data_len = 0
    pending = memoryview(initial)
    if len(pending) > byte_count:
        return byte_count, bytes(pending[byte_count:])
    data_len += len(pending)
    leftover = b""

    while data_len < byte_count:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            raise TimeoutError(f"timed out receiving payload: {data_len}/{byte_count} bytes")
        ready, _, _ = select.select([sock], [], [], min(0.25, remaining))
        if not ready:
            continue
        chunk = sock.recv(min(65536, byte_count - data_len))
        if not chunk:
            raise ConnectionError("socket closed during payload")
        data_len += len(chunk)

    return data_len, leftover


def read_tail(sock, initial=b"", timeout_s=5.0):
    deadline = time.perf_counter() + timeout_s
    data = bytearray(initial)
    while b"\n" not in data:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            break
        ready, _, _ = select.select([sock], [], [], min(0.25, remaining))
        if not ready:
            continue
        chunk = sock.recv(1024)
        if not chunk:
            break
        data.extend(chunk)
    return bytes(data)


def get_local_ip_for(host):
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
      probe.connect((host, 5000))
      return probe.getsockname()[0]
    finally:
      probe.close()


def parse_field(text, name):
    match = re.search(rf"{name}=([0-9]+)", text)
    if not match:
        return None
    return int(match.group(1))


def tcp_test(args):
    sock = socket.create_connection((args.host, args.tcp_port), timeout=args.timeout)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    try:
        command_name = "TCPTHRZ" if args.no_fill else "TCPTHR"
        command = f"{command_name} {args.tcp_mib}\r\n".encode("ascii")
        print(f"TCP: sending {command.decode().strip()} to {args.host}:{args.tcp_port}")
        sock.sendall(command)

        header_bytes, initial_payload = recv_until(sock, BEGIN_MARK, args.timeout)
        header = header_bytes.decode("ascii", errors="replace")
        print("TCP header:")
        print(header.rstrip())

        expected = parse_field(header, "BYTES")
        if expected is None:
            raise RuntimeError("TCP header has no BYTES field")

        start = time.perf_counter()
        received, leftover = recv_exact(sock, expected, initial_payload, timeout_s=args.tcp_timeout)
        elapsed = time.perf_counter() - start
        tail = read_tail(sock, leftover, timeout_s=5.0).decode("ascii", errors="replace").strip()

        print(f"TCP received: {received} bytes in {elapsed:.3f}s, {mbps(received, elapsed):.2f} Mbps")
        if tail:
            board_ms = parse_field(tail, "MS")
            board_bytes = parse_field(tail, "BYTES")
            print(f"TCP board tail: {tail}")
            if board_ms and board_bytes:
                print(f"TCP board rate: {mbps(board_bytes, board_ms / 1000.0):.2f} Mbps")
    finally:
        sock.close()


def udp_test(args):
    local_ip = args.local_ip or get_local_ip_for(args.host)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.udp_rcvbuf)
    udp.bind(("", args.udp_port))
    udp.settimeout(0.1)
    actual_rcvbuf = udp.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)

    tcp = socket.create_connection((args.host, args.tcp_port), timeout=args.timeout)
    tcp.settimeout(0.25)

    command_name = "UDPTHRZ" if args.no_fill else "UDPTHR"
    command = f"{command_name} {local_ip} {args.udp_port} {args.udp_ms} {args.udp_payload}\r\n".encode("ascii")
    print(f"UDP: listening on {local_ip}:{args.udp_port}, SO_RCVBUF={actual_rcvbuf}")
    print(f"UDP: sending {command.decode().strip()} to {args.host}:{args.tcp_port}")

    stats = {
        "packet_count": 0,
        "byte_count": 0,
        "first_time": None,
        "last_time": None,
        "last_seq": None,
        "seq_gap": 0,
        "bad_magic": 0,
    }
    stop_event = threading.Event()

    def udp_receiver():
        while not stop_event.is_set():
            try:
                packet, _ = udp.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break

            now = time.perf_counter()
            if stats["first_time"] is None:
                stats["first_time"] = now
            stats["last_time"] = now
            stats["packet_count"] += 1
            stats["byte_count"] += len(packet)

            if len(packet) >= 16 and packet[:4] == b"N6TP":
                seq = struct.unpack_from("<I", packet, 4)[0]
                last_seq = stats["last_seq"]
                if last_seq is not None and seq != ((last_seq + 1) & 0xFFFFFFFF):
                    stats["seq_gap"] += (seq - last_seq - 1) & 0xFFFFFFFF
                stats["last_seq"] = seq
            else:
                stats["bad_magic"] += 1

    receiver = threading.Thread(target=udp_receiver, name="udp-throughput-recv", daemon=True)
    receiver.start()

    try:
        tcp.sendall(command)
        tcp_text = bytearray()
        deadline = time.perf_counter() + (args.udp_ms / 1000.0) + args.timeout

        while time.perf_counter() < deadline:
            try:
                chunk = tcp.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            tcp_text.extend(chunk)
            if b"END UDPTHR" in tcp_text:
                break

        time.sleep(args.udp_drain_s)
        stop_event.set()
        receiver.join(timeout=1.0)

        first_time = stats["first_time"]
        last_time = stats["last_time"]
        elapsed = 0.0 if first_time is None or last_time is None else max(last_time - first_time, 1e-9)
        byte_count = stats["byte_count"]
        packet_count = stats["packet_count"]
        seq_gap = stats["seq_gap"]
        bad_magic = stats["bad_magic"]

        print(f"UDP received: {byte_count} bytes, packets={packet_count}, gaps={seq_gap}, bad_magic={bad_magic}")
        print(f"UDP PC arrival-window rate: {mbps(byte_count, elapsed):.2f} Mbps over {elapsed:.3f}s")

        text = tcp_text.decode("ascii", errors="replace").strip()
        if text:
            print("UDP board text:")
            print(text)
            board_ms = parse_field(text, "MS")
            board_bytes = parse_field(text, "BYTES")
            board_packets = parse_field(text, "PACKETS")
            if board_ms and board_bytes:
                print(f"UDP board rate: {mbps(board_bytes, board_ms / 1000.0):.2f} Mbps")
                print(f"UDP PC rate over board window: {mbps(byte_count, board_ms / 1000.0):.2f} Mbps")
                if board_bytes:
                    print(f"UDP receive ratio: {byte_count / board_bytes * 100.0:.1f}%")
                if board_packets is not None:
                    print(f"UDP trailing/missing packets: {max(board_packets - packet_count, 0)}")
    finally:
        stop_event.set()
        tcp.close()
        udp.close()


def main():
    parser = argparse.ArgumentParser(description="STM32N6 NetX TCP/UDP throughput test")
    parser.add_argument("--host", default="192.168.1.50")
    parser.add_argument("--tcp-port", type=int, default=5000)
    parser.add_argument("--mode", choices=["tcp", "udp", "both"], default="both")
    parser.add_argument("--tcp-mib", type=int, default=32)
    parser.add_argument("--tcp-timeout", type=float, default=120.0)
    parser.add_argument("--udp-ms", type=int, default=5000)
    parser.add_argument("--udp-port", type=int, default=5010)
    parser.add_argument("--udp-payload", type=int, default=1472)
    parser.add_argument("--udp-rcvbuf", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--udp-drain-s", type=float, default=1.0)
    parser.add_argument("--local-ip", default=None, help="PC IPv4 address for UDPTHR; auto-detected by default")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--no-fill", action="store_true", help="Use TCPTHRZ/UDPTHRZ to reserve packet payload without filling it")
    args = parser.parse_args()

    if args.mode in ("tcp", "both"):
        tcp_test(args)
    if args.mode in ("udp", "both"):
        udp_test(args)


if __name__ == "__main__":
    main()
