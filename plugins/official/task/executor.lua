local ctx = ...
local limit = ctx.arguments and ctx.arguments.limit
if math.type(limit) ~= "integer" or limit < 0 or limit > 1000000 then
  return nil
end

local candidate, multiple, count, completed, sequence = 1, 1, 0, 0, 1
local phase = "queued"
local composite = {}
local cancellation
local description = {
  viewId = ctx.instanceId, revision = ctx.epoch, title = "Count primes",
  unit = "candidates classified", cancellable = true,
}

local function finished()
  return phase == "succeeded" or phase == "cancelled"
end

local function step()
  if finished() then
    return
  end
  sequence = sequence + 1
  if phase == "cancelling" then
    phase = "cancelled"
    composite = nil
    return
  end
  phase = "running"
  for _ = 1, 512 do
    if candidate >= 2 and multiple <= limit then
      composite[multiple] = true
      multiple = multiple + candidate
    else
      candidate = candidate + 1
      if candidate > limit then
        phase = "succeeded"
        composite = nil
        break
      end
      completed = completed + 1
      if not composite[candidate] then
        count = count + 1
        multiple = candidate * candidate
      else
        multiple = limit + 1
      end
    end
  end
end

local function describe()
  return description
end

local function current(input)
  return type(input) == "table" and input.viewId == description.viewId
    and input.revision == description.revision
end

local function invalid(message)
  return nil, { code = "INVALID_INPUT", message = message, effect = "none" }
end

local function read(input)
  if not current(input) then
    return invalid("Stale task request")
  end
  local message = "Count primes using a bounded sieve"
  local result = { kind = "none" }
  if phase == "succeeded" then
    message = "Prime counting complete"
    result = { kind = "success", summary = string.format("%d primes up to %d", count, limit) }
  elseif phase == "cancelled" then
    message = "Prime counting cancelled"
    result = { kind = "cancelled", effect = "none" }
  elseif phase == "cancelling" then
    message = "Cancellation requested; waiting for worker"
  end
  return {
    viewId = description.viewId, revision = description.revision,
    sequence = string.format("%d", sequence), phase = phase,
    completed = string.format("%d", completed),
    total = { kind = "known", units = string.format("%d", math.max(0, limit - 1)) },
    message = message, result = result,
  }
end

local function cancel(input)
  if not current(input) or type(input.requestId) ~= "string" or
      #input.requestId == 0 or #input.requestId > 64 then
    return invalid("Stale or invalid cancellation request")
  end
  if cancellation then
    if cancellation.requestId ~= input.requestId then
      return invalid("A cancellation request already owns this task")
    end
    return cancellation
  end
  local kind = finished() and "alreadyFinished" or "requested"
  if kind == "requested" then
    phase = "cancelling"
    sequence = sequence + 1
  end
  cancellation = {
    viewId = description.viewId, revision = description.revision,
    requestId = input.requestId, kind = kind,
  }
  return cancellation
end

ctx.onStep(step)
return {
  ["view.task.describe"] = describe,
  ["view.task.read"] = read,
  ["view.task.cancel"] = cancel,
}
