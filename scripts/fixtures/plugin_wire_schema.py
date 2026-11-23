"""Independent Draft 2020-12 validator for the bridge schema fixtures."""
import re

from jsonschema import Draft202012Validator, ValidationError, validators


def check_pattern(validator, pattern, instance, schema):
    # ECMA-262's end assertion is absolute when multiline mode is disabled.
    if pattern.endswith('$') and not pattern.endswith(r'\$'):
        pattern = pattern[:-1] + r'\Z'
    if isinstance(instance, str) and not re.search(pattern, instance):
        yield ValidationError(f'{instance!r} does not match {schema["pattern"]!r}')


def validator_for(schema):
    Draft202012Validator.check_schema(schema)
    validator_type = validators.extend(Draft202012Validator, {'pattern': check_pattern})
    return validator_type(schema)
