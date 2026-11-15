// Locked development-only schema checks. Never imports a plugin or starts plugin/network operations.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const require = createRequire(path.join(path.resolve(process.argv[2]), 'package.json'));
if (require('ajv/package.json').version !== '8.17.1') throw new Error('TOOLS_VERSION');
const bytes = fs.readFileSync(process.argv[3]);
if (bytes.length > 262144) throw new Error('SCENARIO_CHECK_LIMIT');
const { schemas, checks } = JSON.parse(bytes);
if (!Array.isArray(schemas) || schemas.length > 16 ||
    checks !== null && (!Array.isArray(checks) || checks.length !== schemas.length)) throw new Error('SCENARIO_CHECKS');
const Ajv = require('ajv/dist/2020').default;
// compile(), rather than compileAsync(): external references are never fetched.
const ajv = new Ajv({ strict: true, allErrors: false, validateFormats: false, coerceTypes: false, useDefaults: false, removeAdditional: false });
const validators = schemas.map(schema => ajv.compile(schema));
if (validators.some(validate => validate.$async)) throw new Error('SCENARIO_ASYNC_SCHEMA_UNSUPPORTED');
if (checks !== null && validators.some((validate, index) => !validate(checks[index]))) throw new Error('TEST_SCENARIO_SCHEMA_RESULT');
console.log(JSON.stringify({ validated: schemas.length, checked: checks?.length ?? 0 }));
