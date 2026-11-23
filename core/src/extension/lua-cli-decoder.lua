local schema, argv = ...
local cli = schema["x-ghosterm-cli"]
if cli == nil then
  assert(#argv == 0)
  return nil
end
assert(schema.type == "object" and schema.additionalProperties == false)
assert(type(schema.properties) == "table" and type(schema.required) == "table")
assert(cli.version == 1 and type(cli.fields) == "table")

local positions, options, decoded, seen = {}, {}, {}, {}
local position_count, field_count = 0, 0
local function checked_value(property, value)
  if property.type == "string" then
    assert(type(value) == "string")
    local length = utf8.len(value)
    assert(length and length >= (property.minLength or 0)
      and length <= (property.maxLength or 4096))
  elseif property.type == "integer" then
    assert(math.type(value) == "integer" and value >= -9007199254740991
      and value <= 9007199254740991)
    assert((property.minimum == nil or value >= property.minimum)
      and (property.maximum == nil or value <= property.maximum))
  else
    assert(property.type == "boolean" and type(value) == "boolean")
  end
  return value
end

for name, field in pairs(cli.fields) do
  local property = schema.properties[name]
  assert(type(name) == "string" and type(property) == "table" and type(field) == "table")
  field_count = field_count + 1
  assert(field_count <= 32)
  if field.position ~= nil then
    assert(math.type(field.position) == "integer" and field.position >= 0
      and field.position < 32 and field.option == nil and field.default == nil)
    local slot = field.position + 1
    assert(positions[slot] == nil)
    positions[slot] = name
    position_count = position_count + 1
  else
    assert(type(field.option) == "string" and string.match(field.option, "^[a-z][a-z0-9-]*$")
      and options[field.option] == nil and field.default ~= nil)
    options[field.option] = name
    decoded[name] = checked_value(property, field.default)
  end
end
for i = 1, position_count do
  assert(positions[i] ~= nil)
end

local i, position, literal = 1, 0, false
while i <= #argv do
  local argument = argv[i]
  if not literal and argument == "--" then
    literal = true
  else
    local name, value
    if not literal and string.sub(argument, 1, 2) == "--" then
      local equals = string.find(argument, "=", 3, true)
      local option = string.sub(argument, 3, equals and equals - 1 or -1)
      name = assert(options[option])
      if equals then
        value = string.sub(argument, equals + 1)
      elseif schema.properties[name].type == "boolean" then
        value = "true"
      else
        i = i + 1
        value = assert(argv[i])
        assert(string.sub(value, 1, 2) ~= "--")
      end
    else
      position = position + 1
      name = assert(positions[position])
      value = argument
    end
    assert(not seen[name])
    seen[name] = true
    local property = schema.properties[name]
    if property.type == "integer" then
      assert(string.match(value, "^%-?%d+$") and not string.match(value, "^%-?0%d")
        and value ~= "-0")
      value = tonumber(value)
    elseif property.type == "boolean" then
      assert(value == "true" or value == "false")
      value = value == "true"
    end
    decoded[name] = checked_value(property, value)
  end
  i = i + 1
end
assert(position == position_count)
for _, name in ipairs(schema.required) do
  assert(decoded[name] ~= nil)
end
return decoded
