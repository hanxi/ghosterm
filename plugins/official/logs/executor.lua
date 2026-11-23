local ctx = ...
local count = ctx.arguments and ctx.arguments.count
if math.type(count) ~= "integer" or count < 1 or count > 10000 then
  return nil
end

local total = math.min(count, 4096)
local oldest = count - total + 1
local page_size = 4
local view_id, revision = ctx.instanceId, ctx.epoch

local function invalid()
  return nil, { code = "INVALID_INPUT", message = "Stale or invalid log query", effect = "none" }
end

local function message(sequence)
  local level = sequence % 97 == 0 and "WARN" or "INFO"
  return string.format("%s worker %d completed sample record %d", level, sequence % 8, sequence)
end

local function contains(text, query)
  for i = 1, #text - #query + 1 do
    if string.sub(text, i, i + #query - 1) == query then
      return true
    end
  end
  return false
end

local function scan(after, through, limit, query)
  if math.type(after) ~= "integer" or math.type(through) ~= "integer"
      or math.type(limit) ~= "integer" or limit < 1 or limit > page_size
      or type(query) ~= "string" then
    return invalid()
  end
  local length = utf8.len(query)
  if through == 0 then
    through = count
  end
  if not length or length > 64 or after < 0 or through < after or through > count then
    return invalid()
  end
  local truncated = after < through and after + 1 < oldest
  local cursor = math.min(through, math.max(after, oldest - 1))
  local ending = math.min(through, cursor + 128)
  local entries = ctx.json.array()
  while cursor < ending and #entries < limit do
    cursor = cursor + 1
    local text = message(cursor)
    if contains(text, query) then
      entries[#entries + 1] = { sequence = cursor, message = text }
    end
  end
  return { entries = entries, cursor = cursor, oldest = oldest, latest = through, truncated = truncated }
end

local function rows(entries)
  local result = ctx.json.array()
  for i = 1, #entries do
    local entry = entries[i]
    local id = string.format("%d", entry.sequence)
    result[i] = { id = id, cells = { id, entry.message } }
  end
  return result
end

local function current(input)
  return type(input) == "table" and input.viewId == view_id and input.revision == revision
end

local function describe()
  return {
    viewId = view_id, revision = revision, title = "Sample logs", total = total, pageSize = page_size,
    columns = { { id = "sequence", title = "Sequence" }, { id = "message", title = "Message" } },
  }
end

local function page(input)
  if not current(input) or math.type(input.offset) ~= "integer"
      or input.offset < 0 or input.offset > total then
    return invalid()
  end
  local result, err = scan(oldest - 1 + input.offset, count, input.limit, "")
  if not result then
    return nil, err
  end
  return { viewId = view_id, revision = revision, offset = input.offset,
           total = total, rows = rows(result.entries) }
end

local function search(input)
  if not current(input) or input.query == "" then
    return invalid()
  end
  local result, err = scan(input.after, input.through, page_size, input.query)
  if not result then
    return nil, err
  end
  return { viewId = view_id, revision = revision, cursor = result.cursor, latest = result.latest,
           truncated = result.truncated, rows = rows(result.entries) }
end

local function log_page(input)
  if type(input) ~= "table" then
    return invalid()
  end
  return scan(input.after, input.through, input.limit, input.query)
end

return {
  ["view.table.describe"] = describe,
  ["view.table.page"] = page,
  ["view.table.search"] = search,
  ["logs.page"] = log_page,
}
