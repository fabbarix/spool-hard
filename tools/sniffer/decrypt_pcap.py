#!/usr/bin/env python3
"""WPA2-PSK CCMP decryption for the spoolhard sniffer pcap.

Extracts the 4-way handshake from each client's session, derives the PTK,
and rewrites CCMP-protected data frames as cleartext (linktype 105 still,
but with the protected bit cleared and the 802.2 LLC header restored).

Usage:
    python3 decrypt_pcap.py --in capture.pcap --out capture.dec.pcap \\
        --ssid Porcini --psk 'TheBeast18Pounds!'

Limitations: only handles unicast CCMP; multicast (GTK) not decrypted.
That's fine for the WS link diagnosis — TCP is unicast.
"""
import argparse
import struct
import hashlib
import hmac

from scapy.all import rdpcap, wrpcap
from scapy.layers.dot11 import Dot11, Dot11CCMP, Dot11QoS, RadioTap
from scapy.layers.eap import EAPOL
from cryptography.hazmat.primitives.ciphers.aead import AESCCM


def pmk(psk: str, ssid: str) -> bytes:
    return hashlib.pbkdf2_hmac("sha1", psk.encode(), ssid.encode(), 4096, 32)


def prf(key: bytes, label: bytes, data: bytes, nbytes: int) -> bytes:
    out = b""
    i = 0
    while len(out) < nbytes:
        out += hmac.new(key, label + b"\x00" + data + bytes([i]),
                        hashlib.sha1).digest()
        i += 1
    return out[:nbytes]


def derive_ptk(pmk_: bytes, aa: bytes, spa: bytes,
                anonce: bytes, snonce: bytes) -> bytes:
    a, b = (aa, spa) if aa < spa else (spa, aa)
    n1, n2 = (anonce, snonce) if anonce < snonce else (snonce, anonce)
    return prf(pmk_, b"Pairwise key expansion", a + b + n1 + n2, 64)


def parse_eapol_key(eap_pkt) -> dict:
    """Extract Key Info / Nonce / Key MIC from an EAPOL-Key frame."""
    raw = bytes(eap_pkt)
    # EAPOL header = 4 bytes (Version, Type, Body Length, ...). Type 3 = Key.
    if raw[1] != 3: return None
    body = raw[4:]
    # 802.1X-2004 EAPOL-Key body:
    # Descriptor Type(1) | Key Info(2) | Key Length(2) | Replay Counter(8)
    # | Key Nonce(32) | EAPOL-Key IV(16) | RSC(8) | Reserved(8) | MIC(16)
    # | Key Data Length(2) | Key Data(*)
    if len(body) < 95: return None
    key_info = struct.unpack(">H", body[1:3])[0]
    key_len  = struct.unpack(">H", body[3:5])[0]
    replay   = body[5:13]
    nonce    = body[13:45]
    mic      = body[77:93]
    kdl      = struct.unpack(">H", body[93:95])[0]
    return {
        "key_info": key_info,
        "key_len":  key_len,
        "replay":   replay,
        "nonce":    nonce,
        "mic":      mic,
        "kd_len":   kdl,
        "raw":      raw,
    }


def collect_handshakes(pkts, ap_bssid: bytes):
    """Find each client's 4-way handshake. Returns
    {client_mac: {'anonce': bytes, 'snonce': bytes}}.
    Caller pairs that with the ap_bssid to derive PTK."""
    sessions = {}
    for p in pkts:
        if not p.haslayer(EAPOL): continue
        if not p.haslayer(Dot11): continue
        d = p[Dot11]
        if d.addr2 is None or d.addr1 is None: continue
        # Identify direction. AP→client: A2=BSSID. Client→AP: A1=BSSID.
        a1 = bytes.fromhex(d.addr1.replace(":", ""))
        a2 = bytes.fromhex(d.addr2.replace(":", ""))
        info = parse_eapol_key(p[EAPOL])
        if info is None: continue
        ki = info["key_info"]
        # bit 8 = MIC present, bit 7 = pairwise. Standard messages:
        # M1: Pairwise=1, MIC=0, Ack=1, Install=0, Secure=0
        # M2: Pairwise=1, MIC=1, Ack=0, Install=0, Secure=0
        # M3: Pairwise=1, MIC=1, Ack=1, Install=1, Secure=1 (or 0 in older)
        # M4: Pairwise=1, MIC=1, Ack=0, Install=0, Secure=1
        ack = (ki >> 7) & 1
        mic = (ki >> 8) & 1
        secure = (ki >> 9) & 1
        install = (ki >> 6) & 1
        if a2 == ap_bssid:
            # AP -> client. M1 (no MIC) carries ANonce.
            if not mic:
                sessions.setdefault(a1, {})["anonce"] = info["nonce"]
        elif a1 == ap_bssid:
            # client -> AP. M2 (mic, no install, NOT secure) carries the
            # real SNonce. M4 also matches mic+!install, but is Secure=1
            # and typically carries a zeroed nonce — would clobber.
            if mic and not install and not secure:
                sessions.setdefault(a2, {})["snonce"] = info["nonce"]
    return sessions


def ccmp_decrypt(tk: bytes, frame: bytes) -> bytes:
    """Strip CCMP and decrypt a Dot11 frame.

    Frame layout: 802.11 hdr (24 or 26 with QoS) | CCMP hdr (8) | ciphertext+MIC (8 trailing).
    For QoS data: header is 26 bytes. For non-QoS data: 24.
    """
    fc = frame[0] | (frame[1] << 8)
    ftype = (fc >> 2) & 0x3
    fsubtype = (fc >> 4) & 0xF
    qos = (ftype == 2) and ((fsubtype & 0x8) != 0)
    hdrlen = 26 if qos else 24

    a1 = frame[4:10]
    a2 = frame[10:16]
    a3 = frame[16:22]
    sc = frame[22:24]
    qc = frame[24:26] if qos else b"\x00\x00"

    ccmp = frame[hdrlen:hdrlen + 8]
    pn0, pn1, _, _, pn2, pn3, pn4, pn5 = ccmp
    pn = bytes([pn5, pn4, pn3, pn2, pn1, pn0])

    # ESP32 promiscuous mode appends 4 bytes of footer after the real
    # frame body (the docs claim sig_len includes "FCS" but it's actually
    # a constant footer, not a per-frame CRC). Trim those 4 bytes so the
    # CCM auth tag lands at the actual end of the ciphertext.
    payload = frame[hdrlen + 8:-4]
    if len(payload) < 8: return None  # need at least MIC
    # Build CCM nonce per IEEE 802.11-2012: priority(1) | A2(6) | PN(6)
    priority = qc[0] & 0x0f if qos else 0
    nonce = bytes([priority]) + a2 + pn

    # AAD per IEEE 802.11-2012: FC' | A1 | A2 | A3 | SC' | (A4 if 4-addr) | (QC' if QoS)
    # Mask matches hostap's hdr_to_aad: byte0 & 0x8f, byte1 & 0xc7. Clears
    # Subtype low-3 bits, Retry, PM, MoreData; keeps Protected bit + Order.
    fc_prime = bytes([frame[0] & 0x8f, frame[1] & 0xc7])
    sc_prime = bytes([sc[0] & 0x0f, 0])
    aad = fc_prime + a1 + a2 + a3 + sc_prime
    if qos:
        aad += bytes([qc[0] & 0x0f, 0])

    try:
        ccm = AESCCM(tk, tag_length=8)
        plain = ccm.decrypt(nonce, payload, aad)
    except Exception:
        return None
    return plain


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in",  dest="inp",   required=True)
    ap.add_argument("--out", dest="outp",  required=True)
    ap.add_argument("--ssid", required=True)
    ap.add_argument("--psk",  required=True)
    ap.add_argument("--bssid", default="74:fe:ce:80:4d:12")
    args = ap.parse_args()

    print(f"[dec] loading {args.inp}")
    pkts = rdpcap(args.inp)
    print(f"[dec] {len(pkts)} frames")

    bssid_bytes = bytes.fromhex(args.bssid.replace(":", ""))
    pmk_bytes   = pmk(args.psk, args.ssid)
    print(f"[dec] PMK (first 8B): {pmk_bytes[:8].hex()}")

    sessions = collect_handshakes(pkts, bssid_bytes)
    print(f"[dec] handshakes: {len(sessions)}")
    ptks = {}
    for client_mac, nonces in sessions.items():
        if "anonce" not in nonces or "snonce" not in nonces:
            print(f"  {client_mac.hex()}: incomplete handshake "
                  f"(have {list(nonces.keys())})")
            continue
        ptk = derive_ptk(pmk_bytes, bssid_bytes, client_mac,
                          nonces["anonce"], nonces["snonce"])
        ptks[client_mac] = ptk
        tk = ptk[32:48]
        print(f"  {client_mac.hex()}: PTK derived. TK={tk.hex()}")

    # Walk frames, decrypt CCMP-protected unicast for known clients.
    out_pkts = []
    decrypted = 0
    skipped = 0
    for p in pkts:
        if not p.haslayer(Dot11):
            out_pkts.append(p); continue
        d = p[Dot11]
        fc_protected = (d.FCfield & 0x40) != 0
        if not fc_protected:
            out_pkts.append(p); continue
        a1 = bytes.fromhex(d.addr1.replace(":", "")) if d.addr1 else None
        a2 = bytes.fromhex(d.addr2.replace(":", "")) if d.addr2 else None
        a3 = bytes.fromhex(d.addr3.replace(":", "")) if d.addr3 else None
        # Pick the client side (non-BSSID) of A1/A2.
        client = None
        for x in (a1, a2):
            if x and x != bssid_bytes:
                client = x; break
        if client not in ptks:
            skipped += 1
            out_pkts.append(p); continue
        tk = ptks[client][32:48]
        raw = bytes(p[Dot11])
        plain = ccmp_decrypt(tk, raw)
        if plain is None:
            skipped += 1
            out_pkts.append(p); continue
        # Rebuild a cleartext data frame: header (no CCMP, no Protected bit) + plain
        fc0 = raw[0]
        fc1 = raw[1] & ~0x40  # clear Protected
        ftype = (raw[0] >> 2) & 0x3
        fsubtype = (raw[0] >> 4) & 0xF
        qos = (ftype == 2) and ((fsubtype & 0x8) != 0)
        hdrlen = 26 if qos else 24
        new = bytes([fc0, fc1]) + raw[2:hdrlen] + plain
        # Wrap into a fresh Dot11 packet preserving timestamp
        new_pkt = Dot11(new)
        new_pkt.time = p.time
        out_pkts.append(new_pkt)
        decrypted += 1

    print(f"[dec] decrypted={decrypted} skipped={skipped}")
    wrpcap(args.outp, out_pkts, linktype=105)
    print(f"[dec] wrote {args.outp}")


if __name__ == "__main__":
    main()
