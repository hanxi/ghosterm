// Independent schema-instance oracle; lexical limits are checked by C fixtures.
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import assert from 'node:assert/strict';
const [schemaFile, casesFile, tools] = process.argv.slice(2);
const require = createRequire(path.join(tools, 'package.json'));
const Ajv = require('ajv/dist/2020').default;
const validate = new Ajv({strict: true, allErrors: true, validateFormats: false}).compile(JSON.parse(fs.readFileSync(schemaFile)));
const cases = JSON.parse(fs.readFileSync(casesFile));
for (const test of cases) {
  const bytes = Buffer.from(test.bytes, 'base64');
  const frame = JSON.parse(bytes.toString('utf8'));
  assert.equal(bytes.length <= 65536 && validate(frame), test.valid, test.name + ': ' + JSON.stringify(validate.errors));
}
console.log('PASS Ajv independent reference ' + cases.length + ' instances');
