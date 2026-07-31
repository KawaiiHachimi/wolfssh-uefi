#!/usr/bin/env python3
"""Tiny unprivileged Ethernet/DHCP/TCP proxy for the QEMU UEFI test.

QEMU's stream netdev carries length-prefixed Ethernet frames. This helper
provides just enough of a LAN (ARP, DHCPv4 and one TCP flow) for the firmware
client to reach a host test service without TAP, root, SLIRP or passt.
"""

import argparse
import asyncio
import ipaddress
import os
import struct


SERVER_MAC = bytes.fromhex("525400123456")
BROADCAST_MAC = b"\xff" * 6
GUEST_IP = ipaddress.IPv4Address("10.0.2.15").packed
SERVER_IP = ipaddress.IPv4Address("10.0.2.2").packed
NETMASK = ipaddress.IPv4Address("255.255.255.0").packed
BROADCAST_IP = ipaddress.IPv4Address("255.255.255.255").packed


def checksum(data):
    if len(data) & 1:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def ethernet(destination, source, ethertype, payload):
    return destination + source + struct.pack("!H", ethertype) + payload


def ipv4(source, destination, protocol, payload, ident):
    header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        20 + len(payload),
        ident & 0xFFFF,
        0x4000,
        64,
        protocol,
        0,
        source,
        destination,
    )
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return header + payload


def udp(source_port, destination_port, payload):
    return struct.pack("!HHHH", source_port, destination_port, 8 + len(payload), 0) + payload


def tcp(source_ip, destination_ip, source_port, destination_port, seq, ack,
        flags, payload=b"", options=b""):
    if len(options) % 4:
        options += b"\x01" * (4 - len(options) % 4)
    offset = 5 + len(options) // 4
    header = struct.pack(
        "!HHIIBBHHH",
        source_port,
        destination_port,
        seq & 0xFFFFFFFF,
        ack & 0xFFFFFFFF,
        offset << 4,
        flags,
        65535,
        0,
        0,
    ) + options
    pseudo = source_ip + destination_ip + struct.pack("!BBH", 0, 6, len(header) + len(payload))
    value = checksum(pseudo + header + payload)
    header = header[:16] + struct.pack("!H", value) + header[18:]
    return header + payload


def dhcp_message_type(options):
    index = 0
    while index < len(options):
        code = options[index]
        index += 1
        if code == 0:
            continue
        if code == 255 or index >= len(options):
            break
        length = options[index]
        index += 1
        value = options[index:index + length]
        index += length
        if code == 53 and value:
            return value[0]
    return None


class NetworkProxy:
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.qemu_writer = None
        self.guest_mac = None
        self.guest_port = None
        self.host_reader = None
        self.host_writer = None
        self.host_task = None
        self.host_pending = bytearray()
        self.guest_pending = bytearray()
        self.guest_next = 0
        self.server_next = 0x6A000001
        self.server_unacked_end = None
        self.host_eof = False
        self.fin_sent = False
        self.ip_ident = 1

    async def send_frame(self, frame):
        if self.qemu_writer is None:
            return
        self.qemu_writer.write(struct.pack("!I", len(frame)) + frame)
        await self.qemu_writer.drain()

    async def send_ip(self, destination_mac, source_ip, destination_ip,
                      protocol, payload):
        packet = ipv4(source_ip, destination_ip, protocol, payload, self.ip_ident)
        self.ip_ident += 1
        await self.send_frame(ethernet(destination_mac, SERVER_MAC, 0x0800, packet))

    async def send_tcp(self, flags, payload=b"", options=b""):
        if self.guest_mac is None or self.guest_port is None:
            return
        segment = tcp(
            SERVER_IP,
            GUEST_IP,
            self.port,
            self.guest_port,
            self.server_next,
            self.guest_next,
            flags,
            payload,
            options,
        )
        consumed = len(payload)
        if flags & 0x02:
            consumed += 1
        if flags & 0x01:
            consumed += 1
        if consumed:
            self.server_next = (self.server_next + consumed) & 0xFFFFFFFF
            self.server_unacked_end = self.server_next
        await self.send_ip(self.guest_mac, SERVER_IP, GUEST_IP, 6, segment)

    async def send_ack(self):
        await self.send_tcp(0x10)

    async def send_host_pending(self):
        if self.server_unacked_end is not None:
            return
        if self.host_pending:
            chunk = bytes(self.host_pending[:1200])
            del self.host_pending[:len(chunk)]
            await self.send_tcp(0x18, chunk)
        elif self.host_eof and not self.fin_sent:
            self.fin_sent = True
            await self.send_tcp(0x11)

    async def connect_host(self):
        if self.host_writer is not None:
            return
        self.host_reader, self.host_writer = await asyncio.open_connection(
            self.host, self.port
        )
        print(f"TCP_PROXY_CONNECTED target={self.host}:{self.port}", flush=True)
        if self.guest_pending:
            self.host_writer.write(self.guest_pending)
            self.guest_pending.clear()
            await self.host_writer.drain()
        self.host_task = asyncio.create_task(self.read_host())

    async def read_host(self):
        try:
            while True:
                data = await self.host_reader.read(16384)
                if not data:
                    break
                self.host_pending.extend(data)
                await self.send_host_pending()
        finally:
            self.host_eof = True
            await self.send_host_pending()

    async def handle_arp(self, frame):
        payload = frame[14:]
        if len(payload) < 28:
            return
        hardware, protocol, hlen, plen, operation = struct.unpack("!HHBBH", payload[:8])
        sender_mac = payload[8:14]
        sender_ip = payload[14:18]
        target_ip = payload[24:28]
        if (hardware, protocol, hlen, plen, operation) != (1, 0x0800, 6, 4, 1):
            return
        if target_ip != SERVER_IP:
            return
        self.guest_mac = sender_mac
        reply = struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
        reply += SERVER_MAC + SERVER_IP + sender_mac + sender_ip
        await self.send_frame(ethernet(sender_mac, SERVER_MAC, 0x0806, reply))
        print("ARP_REPLY gateway=10.0.2.2", flush=True)

    async def handle_dhcp(self, frame, ip_payload):
        if len(ip_payload) < 8 + 240:
            return
        source_port, destination_port, length, _ = struct.unpack("!HHHH", ip_payload[:8])
        if (source_port, destination_port) != (68, 67):
            return
        request = ip_payload[8:8 + length - 8]
        if request[236:240] != bytes.fromhex("63825363"):
            return
        message_type = dhcp_message_type(request[240:])
        if message_type not in (1, 3):
            return
        response_type = 2 if message_type == 1 else 5
        xid = request[4:8]
        flags = request[10:12]
        client_mac = request[28:34]
        self.guest_mac = client_mac
        response = b"\x02\x01\x06\x00" + xid
        response += b"\x00\x00" + flags
        response += b"\x00" * 4
        response += GUEST_IP + SERVER_IP + b"\x00" * 4
        response += client_mac + b"\x00" * 10
        response += b"\x00" * 64 + b"\x00" * 128
        response += bytes.fromhex("63825363")
        response += bytes([53, 1, response_type])
        response += bytes([54, 4]) + SERVER_IP
        response += bytes([1, 4]) + NETMASK
        response += bytes([3, 4]) + SERVER_IP
        response += bytes([6, 4]) + SERVER_IP
        response += bytes([51, 4]) + struct.pack("!I", 3600)
        response += bytes([58, 4]) + struct.pack("!I", 1800)
        response += bytes([59, 4]) + struct.pack("!I", 3150)
        response += b"\xff"
        datagram = udp(67, 68, response)
        await self.send_ip(BROADCAST_MAC, SERVER_IP, BROADCAST_IP, 17, datagram)
        label = "OFFER" if response_type == 2 else "ACK"
        print(f"DHCP_{label} address=10.0.2.15 gateway=10.0.2.2", flush=True)

    async def handle_tcp(self, source_mac, source_ip, destination_ip, payload):
        if len(payload) < 20 or destination_ip != SERVER_IP:
            return
        source_port, destination_port, seq, ack, offset, flags, _, _, _ = struct.unpack(
            "!HHIIBBHHH", payload[:20]
        )
        if destination_port != self.port:
            return
        header_length = (offset >> 4) * 4
        if header_length < 20 or header_length > len(payload):
            return
        data = payload[header_length:]
        self.guest_mac = source_mac
        self.guest_port = source_port

        if flags & 0x04:
            return
        if flags & 0x02:
            self.guest_next = (seq + 1) & 0xFFFFFFFF
            await self.connect_host()
            await self.send_tcp(0x12, options=bytes.fromhex("020405b4"))
            print(f"TCP_SYN guest_port={source_port}", flush=True)
            return

        if self.server_unacked_end is not None:
            distance = (ack - self.server_unacked_end) & 0xFFFFFFFF
            if ack == self.server_unacked_end or distance < 0x80000000:
                self.server_unacked_end = None
                await self.send_host_pending()

        if data:
            if seq == self.guest_next:
                self.guest_next = (self.guest_next + len(data)) & 0xFFFFFFFF
                if self.host_writer is None:
                    self.guest_pending.extend(data)
                else:
                    self.host_writer.write(data)
                    await self.host_writer.drain()
            await self.send_ack()

        if flags & 0x01:
            if seq == self.guest_next:
                self.guest_next = (self.guest_next + 1) & 0xFFFFFFFF
            await self.send_ack()
            if self.host_writer is not None:
                self.host_writer.close()

    async def handle_ipv4(self, frame):
        packet = frame[14:]
        if len(packet) < 20 or packet[0] >> 4 != 4:
            return
        header_length = (packet[0] & 0x0F) * 4
        total_length = struct.unpack("!H", packet[2:4])[0]
        if header_length < 20 or total_length > len(packet):
            return
        protocol = packet[9]
        source_ip = packet[12:16]
        destination_ip = packet[16:20]
        payload = packet[header_length:total_length]
        if protocol == 17:
            await self.handle_dhcp(frame, payload)
        elif protocol == 6:
            await self.handle_tcp(frame[6:12], source_ip, destination_ip, payload)

    async def handle_frame(self, frame):
        if len(frame) < 14:
            return
        ethertype = struct.unpack("!H", frame[12:14])[0]
        if ethertype == 0x0806:
            await self.handle_arp(frame)
        elif ethertype == 0x0800:
            await self.handle_ipv4(frame)

    async def qemu_connected(self, reader, writer):
        if self.qemu_writer is not None:
            writer.close()
            return
        self.qemu_writer = writer
        print("QEMU_NET_CONNECTED", flush=True)
        try:
            while True:
                prefix = await reader.readexactly(4)
                length = struct.unpack("!I", prefix)[0]
                if length == 0 or length > 65536:
                    raise ValueError(f"invalid Ethernet frame length {length}")
                frame = await reader.readexactly(length)
                await self.handle_frame(frame)
        except asyncio.IncompleteReadError:
            pass
        finally:
            print("QEMU_NET_DISCONNECTED", flush=True)
            self.qemu_writer = None
            writer.close()


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket")
    parser.add_argument("--listen-port", type=int, default=3333)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2222)
    args = parser.parse_args()

    proxy = NetworkProxy(args.host, args.port)
    if args.socket:
        try:
            os.unlink(args.socket)
        except FileNotFoundError:
            pass
        server = await asyncio.start_unix_server(
            proxy.qemu_connected, path=args.socket
        )
        endpoint = f"socket={args.socket}"
    else:
        server = await asyncio.start_server(
            proxy.qemu_connected, "127.0.0.1", args.listen_port
        )
        endpoint = f"tcp=127.0.0.1:{args.listen_port}"
    print(f"QEMU_NET_PROXY_READY {endpoint}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
