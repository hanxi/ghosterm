local ctx = ...
local args = ctx.arguments
local max_lines, max_text, page_size = 128, 128, 3

local function lines(encoded)
  local text = ctx.json.decode(encoded)
  if type(text) ~= "string" then
    return nil
  end
  local result, start, i = {}, 1, 1
  while i <= #text + 1 do
    local byte = string.byte(text, i)
    if not byte and i == start then
      break
    end
    if not byte or byte == 10 or byte == 13 then
      local tail = i
      local ending = not byte and "none" or byte == 10 and "lf" or "cr"
      if byte == 13 and string.byte(text, i + 1) == 10 then
        ending = "crlf"
        tail = i + 1
      end
      local value = string.sub(text, start, i - 1)
      local length = utf8.len(value)
      if not length or length > max_text or #result == max_lines then
        return nil
      end
      result[#result + 1] = { text = value, ending = ending, key = ending .. "\0" .. value }
      i = tail
      start = tail + 1
    elseif byte < 32 and byte ~= 9 or byte == 127 then
      return nil
    end
    i = i + 1
  end
  return result
end

local function compare(before, after)
  local matrix = { [#before + 1] = {} }
  for j = 1, #after + 1 do
    matrix[#before + 1][j] = 0
  end
  for i = #before, 1, -1 do
    local row, next_row = { [#after + 1] = 0 }, matrix[i + 1]
    local key = before[i].key
    for j = #after, 1, -1 do
      if key == after[j].key then
        row[j] = next_row[j + 1] + 1
      else
        local below, right = next_row[j], row[j + 1]
        row[j] = below >= right and below or right
      end
    end
    matrix[i] = row
  end

  local i, j = 1, 1
  local rows, old_offsets, new_offsets = {}, { [0] = 0 }, { [0] = 0 }
  local added, removed = 0, 0
  while i <= #before or j <= #after do
    local line, kind, old, new
    if i <= #before and j <= #after and before[i].key == after[j].key then
      line, kind, old, new = before[i], "equal", i, j
      i, j = i + 1, j + 1
    elseif i <= #before and (j > #after or matrix[i + 1][j] >= matrix[i][j + 1]) then
      line, kind, old, new = before[i], "delete", i, 0
      i, removed = i + 1, removed + 1
    else
      line, kind, old, new = after[j], "insert", 0, j
      j, added = j + 1, added + 1
    end
    rows[#rows + 1] = { text = line.text, ending = line.ending, kind = kind,
                         beforeLine = old, afterLine = new }
    old_offsets[#rows], new_offsets[#rows] = i - 1, j - 1
  end
  return rows, old_offsets, new_offsets, added, removed
end

if not args then
  return nil
end
local before, after = lines(args.before), lines(args.after)
if not before or not after then
  return nil
end
local rows, old_offsets, new_offsets, added, removed = compare(before, after)
local view_id, revision = ctx.instanceId, ctx.epoch

local function describe()
  return {
    viewId = view_id, revision = revision, title = "Text comparison",
    beforeLabel = args.beforeLabel, afterLabel = args.afterLabel,
    beforeLines = #before, afterLines = #after, added = added, removed = removed,
    total = #rows, pageSize = page_size,
  }
end

local function page(input)
  if type(input) ~= "table" or input.viewId ~= view_id or input.revision ~= revision
      or math.type(input.offset) ~= "integer" or math.type(input.limit) ~= "integer"
      or input.offset < 0 or input.offset > #rows or input.limit < 1 or input.limit > page_size then
    return nil, { code = "INVALID_INPUT", message = "Stale or invalid diff page", effect = "none" }
  end
  local output = ctx.json.array()
  for i = input.offset + 1, math.min(input.offset + input.limit, #rows) do
    output[#output + 1] = rows[i]
  end
  return { viewId = view_id, revision = revision, offset = input.offset, total = #rows,
           beforeOffset = old_offsets[input.offset], afterOffset = new_offsets[input.offset],
           rows = output }
end

return {
  ["view.diff.describe"] = describe,
  ["view.diff.page"] = page,
}
