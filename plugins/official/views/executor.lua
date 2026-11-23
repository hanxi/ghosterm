local ctx = ...
local handlers = {}

local function invalid()
  return nil, { code = "INVALID_INPUT", message = "Stale or invalid view request", effect = "none" }
end

local function current(input, id)
  return type(input) == "table" and input.viewId == id and input.revision == "1"
end

local function number(value)
  return string.format("%d", value)
end

local function table_view(id, title, total, factor)
  local description = {
    viewId = id, revision = "1", title = title, total = total, pageSize = 16,
    columns = { { id = "number", title = "Number" }, { id = "value", title = "Value" } },
  }
  handlers[id .. ".describe"] = function()
    return description
  end
  handlers[id .. ".page"] = function(input)
    if not current(input, id) or math.type(input.offset) ~= "integer"
        or math.type(input.limit) ~= "integer" or input.offset < 0 or input.offset > total
        or input.limit < 1 or input.limit > 16 then
      return invalid()
    end
    local rows = ctx.json.array()
    for i = 1, math.min(input.limit, total - input.offset) do
      local n = input.offset + i
      rows[i] = { id = number(n), cells = { number(n), number(n * factor) } }
    end
    return { viewId = id, revision = "1", offset = input.offset, total = total, rows = rows }
  end
end

table_view("alpha", "Alpha numbers", 64, 1)
table_view("beta", "Beta numbers", 5, 10)

handlers["changes.describe"] = function()
  return {
    viewId = "changes", revision = "1", title = "A read-only change",
    beforeLabel = "Before", afterLabel = "After", beforeLines = 1, afterLines = 1,
    added = 1, removed = 1, total = 2, pageSize = 2,
  }
end

handlers["changes.page"] = function(input)
  if not current(input, "changes") or input.offset ~= 0 or input.limit ~= 2 then
    return invalid()
  end
  return {
    viewId = "changes", revision = "1", offset = 0, total = 2, beforeOffset = 0, afterOffset = 0,
    rows = {
      { kind = "delete", beforeLine = 1, afterLine = 0, text = "before", ending = "none" },
      { kind = "insert", beforeLine = 0, afterLine = 1, text = "after", ending = "none" },
    },
  }
end

local completed, sequence, phase = 0, 1, "running"
local acknowledgement

handlers["work.describe"] = function()
  return { viewId = "progress", revision = "1", title = "Internal counter",
           unit = "steps", cancellable = true }
end

handlers["work.read"] = function(input)
  if not current(input, "progress") then
    return invalid()
  end
  local result = { kind = "none" }
  if phase == "cancelled" then
    result = { kind = "cancelled", effect = "none" }
  elseif phase == "succeeded" then
    result = { kind = "success", summary = "Counter complete" }
  end
  return {
    viewId = "progress", revision = "1", sequence = number(sequence),
    phase = phase, completed = number(completed), total = { kind = "known", units = "1000000" },
    message = "Counting without external effects", result = result,
  }
end

handlers["work.cancel"] = function(input)
  if not current(input, "progress") or type(input.requestId) ~= "string"
      or #input.requestId == 0 or #input.requestId > 64 then
    return invalid()
  end
  if acknowledgement then
    if acknowledgement.requestId ~= input.requestId then
      return invalid()
    end
    return acknowledgement
  end
  acknowledgement = {
    viewId = "progress", revision = "1", requestId = input.requestId,
    kind = phase == "succeeded" and "alreadyFinished" or "requested",
  }
  if phase == "running" then
    phase, sequence = "cancelling", sequence + 1
  end
  return acknowledgement
end

handlers["views.reopen"] = function(input)
  if type(input) ~= "table" or type(input.viewId) ~= "string" or #input.viewId > 64 then
    return invalid()
  end
  local value = ctx.rpc.call("views.client.open", { viewId = input.viewId }, 5000)
  if type(value) ~= "table" or type(value.opened) ~= "boolean" then
    return nil, { code = "IO", message = "Client registration unconfirmed", effect = "possible" }
  end
  return value
end

ctx.onStep(function()
  if phase == "cancelling" then
    phase, sequence = "cancelled", sequence + 1
  elseif phase == "running" then
    completed, sequence = completed + 1, sequence + 1
    if completed == 1000000 then
      phase = "succeeded"
    end
  end
end)
return handlers
