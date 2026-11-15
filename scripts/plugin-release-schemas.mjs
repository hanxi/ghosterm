import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import path from 'node:path';

const request = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const require = createRequire(path.join(request.tools, 'package.json'));
if (require('ajv/package.json').version !== '8.17.1') throw new Error('RELEASE_SCHEMA_TOOLS');
const Ajv = require('ajv/dist/2020.js').default;
const ajv = new Ajv({ strict: true, coerceTypes: false, useDefaults: false, removeAdditional: false });
const check = (name, value) => ajv.compile(JSON.parse(readFileSync(new URL(`../packages/plugin-sdk/schema/${name}-v1.candidate.json`, import.meta.url), 'utf8')))(value);
process.stdout.write(JSON.stringify({ valid: check('channel', request.policy) &&
  (request.descriptor === null || check('release', request.descriptor)) }));
