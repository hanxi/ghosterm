// Candidate developer testkit. Pure metadata only: no filesystem, install,
// JS activation, network, native handles, or permission grants. A future native
// owner must supply a catalog bound to its verified immutable package snapshot.
export class PackageCheckError extends Error {
  constructor(code, state = 'invalid') {
    super(code);
    this.name = 'PackageCheckError';
    this.code = code;
    this.state = state;
  }
}

function fail(code, state) { throw new PackageCheckError(code, state); }
const reserved = new Set(['plugin', 'doctor', 'relay']);
const excluded = new Set(['integrity.json', 'signature']);
const pathPattern = /^(?!\/)(?!.*\/$)(?!.*[\\\x00-\x1f\x7f:])(?!.*(?:^|\/)\.{1,2}(?:\/|$))(?!.*\/\/)(?!.*[. ](?:\/|$)).+$/u;

export function canonicalMemberPath(value) {
  if (typeof value !== 'string' || value.length > 4096 || !pathPattern.test(value) ||
      [...value].length > 1024 || value.normalize('NFC') !== value ||
      [...value].some(char => { const code = char.codePointAt(0); return code >= 0xd800 && code <= 0xdfff; })) {
    fail('MEMBER_PATH');
  }
  return value;
}

function indexMembers(members) {
  if (!Array.isArray(members) || members.length > 1024) fail('MEMBER_COUNT');
  const byPath = new Map();
  const byFold = new Map();
  let total = 0;
  for (const member of members) {
    if (!member || typeof member !== 'object' || Array.isArray(member)) fail('MEMBER_SHAPE');
    const path = canonicalMemberPath(member.path);
    if (byPath.has(path)) fail('DUPLICATE_MEMBER');
    const folded = path.toLowerCase();
    if (byFold.has(folded)) fail('CASE_COLLISION');
    if (excluded.has(folded) && path !== folded) fail('RESERVED_MEMBER_CASE');
    if (member.kind !== 'regular') fail('NONREGULAR_MEMBER');
    if (!Number.isSafeInteger(member.bytes) || member.bytes < 0 || member.bytes > 16777216) fail('MEMBER_BYTES');
    if (typeof member.sha256 !== 'string' || !/^[0-9a-f]{64}$/.test(member.sha256)) fail('MEMBER_DIGEST');
    total += member.bytes;
    if (total > 67108864) fail('PACKAGE_BYTES');
    byPath.set(path, member);
    byFold.set(folded, path);
  }
  for (const path of byPath.keys()) {
    const parts = path.toLowerCase().split('/');
    for (let i = 1; i < parts.length; i++) {
      if (byFold.has(parts.slice(0, i).join('/'))) fail('FILE_DIRECTORY_COLLISION');
    }
  }
  return byPath;
}

export function commandConflicts(manifests) {
  const found = [];
  const commands = [];
  for (const manifest of manifests) {
    for (const command of manifest.commands) {
      if (reserved.has(command.path[0])) fail('RESERVED_COMMAND');
      for (const previous of commands) {
        const length = Math.min(previous.path.length, command.path.length);
        if (previous.path.slice(0, length).every((token, index) => token === command.path[index])) {
          found.push({ first: previous.pluginId, second: manifest.id, path: command.path.slice(0, length) });
        }
      }
      commands.push({ pluginId: manifest.id, path: command.path });
    }
  }
  return found;
}

export function createPackageInspector({ validateManifest, validateIntegrity, semver }) {
  function range(value) {
    if (!semver.validRange(value)) fail('VERSION_RANGE');
  }
  function references(manifest, members) {
    const requireMember = path => {
      canonicalMemberPath(path);
      if (!members.has(path) || excluded.has(path)) fail('MISSING_REFERENCED_MEMBER');
    };
    const script = path => {
      requireMember(path);
      if (!/\.(?:mjs|js)$/.test(path)) fail('SCRIPT_ENTRY');
    };
    for (const command of manifest.commands) requireMember(command.argsSchema);
    if (manifest.client) script(manifest.client.entry);
    const ids = new Set();
    for (const variant of manifest.executor?.variants ?? []) {
      if (ids.has(variant.id)) fail('DUPLICATE_VARIANT');
      ids.add(variant.id);
      if (variant.runtime === 'exec') requireMember(variant.executable);
      else script(variant.entry);
      if (variant.runtime === 'node') range(variant.runtimeVersionRange);
      const programs = new Set();
      for (const dependency of variant.dependencies ?? []) {
        if (programs.has(dependency.program)) fail('DUPLICATE_DEPENDENCY');
        programs.add(dependency.program);
        if (dependency.versionPolicy === 'adapter-verified') requireMember(dependency.schema);
        else range(dependency.versionRange);
      }
    }
  }
  function declarations(manifest) {
    const permissionKeys = new Set();
    const sides = { client: manifest.client ? [manifest.client] : [], executor: manifest.executor?.variants ?? [] };
    for (const permission of manifest.permissions) {
      const key = permission.side + ':' + permission.capability;
      if (permissionKeys.has(key)) fail('DUPLICATE_PERMISSION');
      permissionKeys.add(key);
      if (!sides[permission.side].some(side => [...side.requires, ...(side.optional ?? [])].includes(permission.capability))) {
        fail('UNDECLARED_PERMISSION');
      }
    }
    for (const [name, variants] of Object.entries(sides)) {
      for (const variant of variants) {
        if ((variant.optional ?? []).some(capability => variant.requires.includes(capability))) fail('CAPABILITY_OVERLAP');
        if (name === 'client' && [...variant.requires, ...(variant.optional ?? [])].some(capability =>
          capability.startsWith('process.') || capability.startsWith('file.'))) fail('CLIENT_EXECUTOR_CAPABILITY');
        for (const capability of [...variant.requires, ...(variant.optional ?? [])]) {
          // process.pipes is availability of the owned-child pipe service;
          // the spawn permission scopes the child/program. It is not a second
          // ambient process permission (matches the plan's Codex manifest).
          if (/^(?:process\.spawn$|file\.|network\.|credential\.|local\.|upload(?:\.|$))/.test(capability) &&
              !permissionKeys.has(name + ':' + capability)) fail('MISSING_PERMISSION_DECLARATION');
        }
      }
    }
  }
  function programFact(host, program) {
    const fact = host.programs?.[program];
    if (!fact || fact.state === 'unknown') fail('DEPENDENCY_UNKNOWN', 'unknown');
    if (fact.state === 'missing') fail('DEPENDENCY_UNSUPPORTED', 'unsupported');
    if (fact.state === 'denied') fail('DEPENDENCY_DENIED', 'denied');
    if (fact.state !== 'available' || !semver.valid(fact.version)) fail('DEPENDENCY_UNKNOWN', 'unknown');
    return fact;
  }
  return function inspect(manifest, integrity, observed, host) {
    if (!validateManifest(manifest)) fail('MANIFEST_SCHEMA');
    if (!validateIntegrity(integrity)) fail('INTEGRITY_SCHEMA');
    const members = indexMembers(observed);
    const declared = new Set();
    for (const member of integrity.members) {
      const path = canonicalMemberPath(member.path);
      if (excluded.has(path)) fail('INTEGRITY_SELF_REFERENCE');
      if (declared.has(path)) fail('DUPLICATE_INTEGRITY_MEMBER');
      declared.add(path);
      const actual = members.get(path);
      if (!actual) fail('MISSING_MEMBER');
      if (actual.bytes !== member.bytes || actual.sha256 !== member.sha256) fail('MEMBER_MISMATCH');
    }
    for (const path of members.keys()) {
      if (!excluded.has(path) && !declared.has(path)) fail('UNINDEXED_MEMBER');
    }
    if (!members.has('integrity.json') || !members.has('plugin.json') ||
        !(members.has('README') || members.has('README.md')) ||
        ![...members.keys()].some(path => path.startsWith('LICENSES/'))) fail('REQUIRED_PACKAGE_MEMBER');
    range(manifest.sdkApiRange);
    references(manifest, members);
    declarations(manifest);
    if (commandConflicts([manifest]).length) fail('COMMAND_CONFLICT');
    if (!host || !['client', 'executor'].includes(host.side) || !semver.valid(host.sdkVersion)) fail('HOST_UNKNOWN', 'unknown');
    if (!semver.satisfies(host.sdkVersion, manifest.sdkApiRange)) fail('SDK_UNSUPPORTED', 'unsupported');
    if (!Array.isArray(host.protocols) || host.protocols.some(version => !Number.isInteger(version) || version < 1 || version > 65535)) fail('PROTOCOL_UNKNOWN', 'unknown');
    const protocols = manifest.bridgeProtocolRange.filter(version => host.protocols.includes(version));
    if (!protocols.length) fail('PROTOCOL_UNSUPPORTED', 'unsupported');
    if (!Array.isArray(host.runtimes) || !Array.isArray(host.capabilities) ||
        [...host.runtimes, ...host.capabilities].some(value => typeof value !== 'string' || !value) ||
        (host.deniedCapabilities !== undefined && (!Array.isArray(host.deniedCapabilities) ||
          host.deniedCapabilities.some(value => typeof value !== 'string' || !value)))) fail('CAPABILITIES_UNKNOWN', 'unknown');
    let selected;
    if (host.side === 'client') {
      if (!manifest.client) fail('SIDE_UNSUPPORTED', 'unsupported');
      selected = manifest.client;
    } else {
      if (!manifest.executor) fail('SIDE_UNSUPPORTED', 'unsupported');
      if (host.variantId && !manifest.executor.variants.some(variant => variant.id === host.variantId)) fail('VARIANT_UNSUPPORTED', 'unsupported');
      let variants = manifest.executor.variants.filter(variant => !host.variantId || variant.id === host.variantId);
      if (variants.some(variant => (variant.os && !host.os) || (variant.arch && !host.arch))) fail('PLATFORM_UNKNOWN', 'unknown');
      variants = variants.filter(variant => (!variant.os || variant.os.includes(host.os)) && (!variant.arch || variant.arch.includes(host.arch)));
      if (!variants.length) fail('PLATFORM_UNSUPPORTED', 'unsupported');
      // Runtime availability is not a silent variant selection policy.
      if (variants.length !== 1) fail('VARIANT_SELECTION_REQUIRED');
      selected = variants[0];
    }
    if (!host.runtimes.includes(selected.runtime)) fail('RUNTIME_UNSUPPORTED', 'unsupported');
    for (const capability of selected.requires) {
      if (host.deniedCapabilities?.includes(capability)) fail('CAPABILITY_DENIED', 'denied');
      if (!host.capabilities.includes(capability)) fail('CAPABILITY_UNSUPPORTED', 'unsupported');
    }
    if (selected.runtime === 'node') {
      const fact = programFact(host, 'node');
      if (!semver.satisfies(fact.version, selected.runtimeVersionRange)) fail('RUNTIME_VERSION_UNSUPPORTED', 'unsupported');
    }
    for (const dependency of selected.dependencies ?? []) {
      const fact = programFact(host, dependency.program);
      if (dependency.versionPolicy === 'range') {
        if (!semver.satisfies(fact.version, dependency.versionRange)) fail('DEPENDENCY_VERSION_UNSUPPORTED', 'unsupported');
      } else {
        const verified = host.adapters?.[dependency.program];
        if (verified?.state === 'rejected') fail('ADAPTER_UNSUPPORTED', 'unsupported');
        if (verified?.state !== 'verified' || verified.version !== fact.version ||
            verified.schemaDigest !== members.get(dependency.schema).sha256) fail('ADAPTER_UNKNOWN', 'unknown');
      }
    }
    return {
      kind: 'compatible', side: host.side, variantId: selected.id ?? null,
      entry: selected.entry ?? selected.executable, protocols,
      optionalMissing: (selected.optional ?? []).filter(capability => !host.capabilities.includes(capability) || host.deniedCapabilities?.includes(capability)),
      authorization: 'not-granted', publisherAuthenticated: false,
    };
  };
}
