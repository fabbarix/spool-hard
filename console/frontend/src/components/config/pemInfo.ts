// Minimal DER walker: just enough of X.509 / CRL to preview what the user
// staged (validity dates, subject CN) before it goes to the console. Only
// the first PEM block is read (the leaf, in a chain file). The firmware does
// the real validation on install.

interface Tlv { tag: number; start: number; end: number }   // start/end = content span

function readTlv(b: Uint8Array, pos: number): Tlv & { next: number } {
  const tag = b[pos];
  let len = b[pos + 1];
  let p = pos + 2;
  if (len & 0x80) {
    const n = len & 0x7f;
    if (n === 0 || n > 4) throw new Error('bad length');
    len = 0;
    for (let i = 0; i < n; i++) len = len * 256 + b[p++];
  }
  if (p + len > b.length) throw new Error('truncated');
  return { tag, start: p, end: p + len, next: p + len };
}

function children(b: Uint8Array, t: Tlv): Tlv[] {
  const out: Tlv[] = [];
  for (let p = t.start; p < t.end;) {
    const c = readTlv(b, p);
    out.push(c);
    p = c.next;
  }
  return out;
}

const ascii = (b: Uint8Array, t: Tlv) => new TextDecoder().decode(b.subarray(t.start, t.end));

// UTCTime (0x17) YYMMDDHHMMSSZ or GeneralizedTime (0x18) YYYYMMDDHHMMSSZ.
function parseTime(b: Uint8Array, t: Tlv): Date | null {
  const s = ascii(b, t);
  const m = t.tag === 0x17 ? /^(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)?Z$/.exec(s)
          : t.tag === 0x18 ? /^(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)?Z$/.exec(s) : null;
  if (!m) return null;
  let y = Number(m[1]);
  if (t.tag === 0x17) y += y < 50 ? 2000 : 1900;
  return new Date(Date.UTC(y, Number(m[2]) - 1, Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6] ?? 0)));
}

const isTime = (t: Tlv) => t.tag === 0x17 || t.tag === 0x18;

// CN (OID 2.5.4.3) of a Name, else "".
function commonName(b: Uint8Array, name: Tlv): string {
  for (const rdn of children(b, name)) {
    for (const atv of children(b, rdn)) {
      const [oid, val] = children(b, atv);
      if (oid && val && oid.end - oid.start === 3 &&
          b[oid.start] === 0x55 && b[oid.start + 1] === 0x04 && b[oid.start + 2] === 0x03) {
        return ascii(b, val);
      }
    }
  }
  return '';
}

function pemToDer(pem: string): Uint8Array {
  const first = /-----BEGIN [^-]+-----([\s\S]+?)-----END [^-]+-----/.exec(pem);
  const body = (first ? first[1] : pem).replace(/\s+/g, '');
  const bin = atob(body);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}

export interface CertInfo { kind: 'cert'; subject: string; notBefore: Date; notAfter: Date }
export interface CrlInfo { kind: 'crl'; issuer: string; thisUpdate: Date; nextUpdate: Date | null }

export function parseCertPem(pem: string): CertInfo | null {
  try {
    const b = pemToDer(pem);
    const cert = readTlv(b, 0);
    const tbs = children(b, cert)[0];
    let f = children(b, tbs);
    if (f[0]?.tag === 0xa0) f = f.slice(1);          // explicit [0] version
    // serial, signature alg, issuer, validity, subject
    const validity = children(b, f[3]);
    const notBefore = parseTime(b, validity[0]);
    const notAfter = parseTime(b, validity[1]);
    if (!notBefore || !notAfter) return null;
    return { kind: 'cert', subject: commonName(b, f[4]), notBefore, notAfter };
  } catch { return null; }
}

export function parseCrlPem(pem: string): CrlInfo | null {
  try {
    const b = pemToDer(pem);
    const crl = readTlv(b, 0);
    const tbs = children(b, crl)[0];
    let f = children(b, tbs);
    if (f[0]?.tag === 0x02) f = f.slice(1);          // optional version INTEGER
    // signature alg, issuer, thisUpdate, nextUpdate?
    const thisUpdate = isTime(f[2]) ? parseTime(b, f[2]) : null;
    if (!thisUpdate) return null;
    const nextUpdate = f[3] && isTime(f[3]) ? parseTime(b, f[3]) : null;
    return { kind: 'crl', issuer: commonName(b, f[1]), thisUpdate, nextUpdate };
  } catch { return null; }
}

export function daysFromNow(d: Date): number {
  return Math.floor((d.getTime() - Date.now()) / 86_400_000);
}
