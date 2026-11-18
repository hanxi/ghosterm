/* Trusted CLI code, never loaded from a plugin. Same candidate release/channel
 * contracts as the native Apps; exact signed bytes are verified by native C. */
(function(descriptorBytes, policyBytes) {
  'use strict';
  const fail = code => { throw Error(code); };
  function parse(source) {
    let at = 0, nodes = 0;
    const space = () => { while (/^[ \t\r\n]$/.test(source[at] ?? '')) at++; };
    function string() {
      const start = at++;
      while (at < source.length) {
        const c = source.charCodeAt(at++);
        if (c < 32) fail('RELEASE_JSON');
        if (c === 34) {
          const s = JSON.parse(source.slice(start, at));
          for (let i = 0; i < s.length; i++) {
            const u = s.charCodeAt(i);
            if (u >= 0xd800 && u <= 0xdbff) {
              const v = s.charCodeAt(++i);
              if (!(v >= 0xdc00 && v <= 0xdfff)) fail('RELEASE_UTF8');
            } else if (u >= 0xdc00 && u <= 0xdfff) fail('RELEASE_UTF8');
          }
          return s;
        }
        if (c === 92) {
          const escape = source[at++];
          if (escape === 'u') {
            if (!/^[0-9a-fA-F]{4}$/.test(source.slice(at, at + 4))) fail('RELEASE_JSON');
            at += 4;
          } else if (!'"\\/bfnrt'.includes(escape ?? '\0')) fail('RELEASE_JSON');
        }
      }
      fail('RELEASE_JSON');
    }
    function value(depth) {
      if (++nodes > 16384 || depth > 64) fail('RELEASE_LIMIT');
      space(); const c = source[at];
      if (c === '"') return string();
      if (c === '{') {
        at++; space(); const result = Object.create(null), keys = new Set();
        if (source[at] === '}') { at++; return result; }
        for (;;) {
          space(); if (source[at] !== '"') fail('RELEASE_JSON');
          const key = string();
          if (keys.has(key)) fail('RELEASE_DUPLICATE'); keys.add(key);
          space(); if (source[at++] !== ':') fail('RELEASE_JSON');
          result[key] = value(depth + 1); space();
          const end = source[at++]; if (end === '}') return result;
          if (end !== ',') fail('RELEASE_JSON');
        }
      }
      if (c === '[') {
        at++; space(); const result = [];
        if (source[at] === ']') { at++; return result; }
        for (;;) {
          result.push(value(depth + 1)); space();
          const end = source[at++]; if (end === ']') return result;
          if (end !== ',') fail('RELEASE_JSON');
        }
      }
      for (const [literal, result] of [['true', true], ['false', false], ['null', null]]) {
        if (source.slice(at, at + literal.length) === literal) { at += literal.length; return result; }
      }
      const match = /^(0|[1-9][0-9]*)(?:\.([0-9]+))?(?:[eE]([+-]?[0-9]+))?/.exec(source.slice(at));
      if (!match || match[0].length > 256) fail('RELEASE_NUMBER');
      at += match[0].length;
      let digits = match[1] + (match[2] ?? '');
      let scale = Number(match[3] ?? 0) - (match[2]?.length ?? 0);
      if (!Number.isSafeInteger(scale) || Math.abs(scale) > 512) fail('RELEASE_NUMBER');
      while (scale < 0 && digits.endsWith('0')) { digits = digits.slice(0, -1); scale++; }
      if (scale < 0 || digits.length + scale > 256) fail('RELEASE_NUMBER');
      const exact = BigInt(digits || '0') * 10n ** BigInt(scale);
      if (exact > 9007199254740991n) fail('RELEASE_NUMBER');
      return Number(exact);
    }
    const result = value(0); space(); if (at !== source.length) fail('RELEASE_JSON'); return result;
  }
  const exact = (o, keys) => o && !Array.isArray(o) && typeof o === 'object' &&
    Object.keys(o).length === keys.length && keys.every(k => Object.hasOwn(o, k));
  const digest = s => typeof s === 'string' && /^[0-9a-f]{64}$/.test(s);
  const channel = s => typeof s === 'string' && /^[a-z0-9][a-z0-9.-]{0,63}$/.test(s);
  const plugin = s => typeof s === 'string' && s.length <= 128 && /^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$/.test(s);
  const text = s => typeof s === 'string' && [...s].length >= 1 && [...s].length <= 128 &&
    s.normalize('NFC') === s && [...s].every(c => c.codePointAt(0) >= 32 && c.codePointAt(0) !== 127);
  function publicKey(s) {
    if (typeof s !== 'string' || !/^[A-Za-z0-9+/]{43}=$/.test(s)) fail('RELEASE_POLICY');
    const alphabet = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    let bits = 0, count = 0, hex = '';
    for (const c of s.slice(0, -1)) {
      bits = (bits << 6) | alphabet.indexOf(c); count += 6;
      if (count >= 8) { count -= 8; hex += ((bits >>> count) & 255).toString(16).padStart(2, '0'); }
    }
    if (hex.length !== 64 || (bits & 3)) fail('RELEASE_POLICY');
    return hex;
  }
  const p = parse(policyBytes), keys = new Map();
  if (!exact(p, ['schemaVersion','channel','keys','revokedKeyIds']) || p.schemaVersion !== 1 ||
      !channel(p.channel) || !Array.isArray(p.keys) || p.keys.length < 1 || p.keys.length > 64 ||
      !Array.isArray(p.revokedKeyIds) || p.revokedKeyIds.length > 256 ||
      !p.revokedKeyIds.every(digest) || new Set(p.revokedKeyIds).size !== p.revokedKeyIds.length) fail('RELEASE_POLICY');
  for (const k of p.keys) {
    if (!exact(k, ['id','algorithm','publicKey','publisher','pluginIds']) || !digest(k.id) ||
        keys.has(k.id) || k.algorithm !== 'ed25519' || !text(k.publisher) ||
        !Array.isArray(k.pluginIds) || k.pluginIds.length < 1 || k.pluginIds.length > 64 ||
        !k.pluginIds.every(plugin) || new Set(k.pluginIds).size !== k.pluginIds.length) fail('RELEASE_POLICY');
    const raw = publicKey(k.publicKey);
    if (__release_sha256(raw) !== k.id) fail('RELEASE_POLICY');
    keys.set(k.id, {raw, publisher:k.publisher, plugins:k.pluginIds});
  }
  if (descriptorBytes === null) return true;
  const d = parse(descriptorBytes);
  if (!exact(d, ['schemaVersion','channel','keyId','pluginId','pluginVersion','publisher','packageSHA256','artifact']) ||
      d.schemaVersion !== 1 || !channel(d.channel) || !digest(d.keyId) || !plugin(d.pluginId) ||
      !text(d.pluginVersion) || !text(d.publisher) || !digest(d.packageSHA256) ||
      !exact(d.artifact, ['format','bytes','sha256']) || d.artifact.format !== 'zip-v1' ||
      !Number.isSafeInteger(d.artifact.bytes) || d.artifact.bytes < 22 || d.artifact.bytes > 75497472 ||
      !digest(d.artifact.sha256)) fail('RELEASE_DESCRIPTOR');
  if (d.channel !== p.channel) fail('RELEASE_SCOPE');
  const key = keys.get(d.keyId);
  if (!key) fail('RELEASE_UNKNOWN_KEY');
  if (p.revokedKeyIds.includes(d.keyId)) fail('RELEASE_REVOKED');
  if (key.publisher !== d.publisher || !key.plugins.includes(d.pluginId)) fail('RELEASE_SCOPE');
  return {id:d.pluginId, version:d.pluginVersion, publisher:d.publisher, digest:d.packageSHA256,
    key:key.raw, artifact_digest:d.artifact.sha256, artifact_bytes:d.artifact.bytes};
})
