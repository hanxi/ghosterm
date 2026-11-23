local ctx = ...
local request = {
  requestId = "1",
  title = "Choose sample settings",
  fields = {
    { kind = "text", id = "name", label = "Display name", required = true, maxLength = 64 },
    { kind = "toggle", id = "summary", label = "Show a summary" },
    {
      kind = "choice", id = "layout", label = "Layout",
      options = {
        { id = "compact", label = "Compact" },
        { id = "comfortable", label = "Comfortable" },
      },
    },
  },
}

local handle = ctx.rpc.start("view.form.request", request, 120000)
if not handle then
  return nil
end

local status = "pending"

local function valid_name(value)
  if type(value) ~= "string" then
    return false
  end
  local length = utf8.len(value)
  if not length or length == 0 or length > 64 then
    return false
  end
  local nonblank = false
  for _, code in utf8.codes(value) do
    if code < 32 or code == 127 then
      return false
    end
    local blank = code == 32 or code == 0xa0 or code == 0x1680
      or (code >= 0x2000 and code <= 0x200a) or code == 0x2028 or code == 0x2029
      or code == 0x202f or code == 0x205f or code == 0x3000 or code == 0xfeff
    nonblank = nonblank or not blank
  end
  return nonblank
end

local function observe()
  if status ~= "pending" then
    return
  end
  local reply, err = ctx.rpc.poll(handle)
  if reply then
    if type(reply) ~= "table" or reply.requestId ~= request.requestId then
      status = "failed"
    elseif reply.kind == "dismissed" then
      status = "dismissed"
    elseif reply.kind == "submitted" and type(reply.values) == "table" then
      local values = reply.values
      if valid_name(values.name) and type(values.summary) == "boolean"
          and (values.layout == "compact" or values.layout == "comfortable") then
        status = "submitted"
      else
        status = "failed"
      end
    else
      status = "failed"
    end
  elseif err then
    status = err.code == "RPC_CANCELLED" and "cancelled"
      or err.code == "RPC_UNKNOWN" and "unknown" or "failed"
  end
end

local function read_status()
  observe()
  return { status = status }
end

local function cancel()
  observe()
  return { requested = status == "pending" and ctx.rpc.cancel(handle) }
end

return {
  ["form.status"] = read_status,
  ["form.cancel"] = cancel,
}
