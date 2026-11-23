local ctx = ...
if not ctx.process then
  return nil
end

local cli_version = "2.1.323"
local phase, failure, reaped = "new", "", false
local child, prompt, buffer, terminal
local message_id
local line_parts, line_bytes = {}, 0
local line_limit, receipt_limit = 524288, 131072
local written, serial, sequence = 0, 0, 0
local command_sequence = 0
local exit_info = ctx.json.null
local input, receipts, receipt_count = nil, {}, 0
local latest_response, latest_input = "", ""
local events = {}
local handles = {}

local function units(text)
  local points = utf8.len(text)
  if not points then
    return nil
  end
  local count = points
  for _, code in utf8.codes(text) do
    if code > 0xffff then
      count = count + 1
    end
  end
  return count
end

local function identifier(prefix)
  serial = serial + 1
  return prefix .. string.format("%d", serial)
end

local function next_page(text, offset)
  local position, first, ending = 0, nil, #text + 1
  for at, code in utf8.codes(text) do
    if position == offset then
      first = at
    elseif position > offset and not first then
      return nil
    end
    local width = code > 0xffff and 2 or 1
    if first and position + width > offset + 256 then
      ending = at
      break
    end
    position = position + width
  end
  if not first and position == offset then
    return "", offset
  end
  if first then
    return string.sub(text, first, ending - 1), position
  end
end

local function fail(code)
  failure = code
  phase = "failed"
  if child then
    ctx.process.terminate(child)
  end
end

local function clear_output()
  buffer, line_parts, line_bytes = "", {}, 0
end

local function read_line()
  for i = 1, #buffer do
    if string.byte(buffer, i) == 10 then
      if line_bytes + i - 1 > line_limit then
        return nil, true
      end
      line_parts[#line_parts + 1] = string.sub(buffer, 1, i - 1)
      buffer = string.sub(buffer, i + 1)
      local value = ctx.json.decode(table.concat(line_parts), line_limit)
      line_parts, line_bytes = {}, 0
      return value, true
    end
  end
  if #buffer > 0 then
    line_bytes = line_bytes + #buffer
    if line_bytes > line_limit then
      return nil, true
    end
    line_parts[#line_parts + 1] = buffer
    buffer = ""
  end
  return nil, false
end

local function emit_message(message)
  sequence = sequence + 1
  events[#events + 1] = {
    source = "session", event = { kind = "sdk-message", sequence = sequence, message = message },
  }
end

local function ingest(message)
  if type(message) ~= "table" or type(message.type) ~= "string" then
    fail("PROTOCOL")
    return
  end
  if message.type == "result" then
    if terminal or type(message.is_error) ~= "boolean" or type(message.subtype) ~= "string" or
        (not message.is_error and message.subtype ~= "success") then
      fail("PROTOCOL")
      return
    end
    terminal = message
  elseif message.type == "assistant" or message.type == "stream_event" then
    emit_message(message)
  end
end

local function step()
  if not child then
    return
  end
  if prompt and phase == "running" then
    local amount = ctx.process.writeStdin(child, string.sub(prompt, written + 1, written + 3072))
    if not amount then
      prompt = nil
      fail("IO")
    else
      written = written + amount
      if written == #prompt then
        prompt = nil
        if not ctx.process.closeStdin(child) then
          fail("IO")
        end
      end
    end
  end
  ctx.process.readStderr(child, 3072)
  if #events >= 8 and phase == "running" then
    return
  end
  if phase == "running" then
    local message, had_line = read_line()
    if had_line then
      ingest(message)
      return
    end
  end
  local chunk, status = ctx.process.readStdout(child, 3072)
  if chunk then
    if phase == "running" then
      buffer = chunk
    end
    return
  elseif status == "wait" then
    return
  end
  local info = ctx.process.poll(child)
  if not info or not info.exited then
    return
  end
  if not ctx.process.release(child) then
    fail("IO")
    return
  end
  child, reaped = nil, true
  exit_info = { state = "exited", code = info.exitCode, signal = info.termSignal }
  if phase == "interrupting" then
    emit_message({ type = "result", subtype = "error_interrupted", is_error = true,
      user_message_uuid = message_id, result = "Claude CLI interrupted" })
    phase = "ready"
  elseif phase == "stopping" then
    phase = "closed"
  elseif phase == "running" then
    if status == "eof" and line_bytes == 0 and terminal and info.termSignal == 0 and
        (info.exitCode == 0 or terminal.is_error) then
      terminal.user_message_uuid = message_id
      emit_message(terminal)
      phase = "ready"
    else
      fail("CLI_EXIT")
    end
  end
  terminal, message_id = nil, nil
  clear_output()
end

local function start()
  if phase ~= "new" then
    return nil, "busy"
  end
  phase = "ready"
  return { requested = true, phase = phase, backend = "cli", cliVersion = cli_version }
end

local function send(params)
  local count = type(params.text) == "string" and units(params.text)
  if phase ~= "ready" or child then
    return nil, "busy"
  end
  if not count or count == 0 or #params.text > 2048 then
    return nil, "invalid"
  end
  child = ctx.process.spawn("claude", "workspace", {
    "--print", "--input-format", "text", "--output-format", "stream-json", "--verbose",
    "--restricted", "--permission-mode", "default", "--permission-prompts", "none",
    "--tools", "", "--disallowedTools", "mcp__*", "--strict-mcp-config",
    "--mcp-config", '{"mcpServers":{}}', "--no-session-persistence",
  }, cli_version)
  if not child then
    fail("STARTUP")
    return nil, "unavailable"
  end
  phase, prompt, written, terminal, reaped = "running", params.text, 0, nil, false
  message_id = identifier("message-")
  clear_output()
  return { messageID = message_id }
end

local function interrupt()
  if phase ~= "running" or not child then
    return nil, "invalid"
  end
  if not ctx.process.terminate(child) then
    return nil, "unavailable"
  end
  prompt, phase = nil, "interrupting"
  clear_output()
  return { requested = true, phase = phase }
end

local function stop()
  if child then
    local pending = message_id
    if not ctx.process.terminate(child) then
      return nil, "unavailable"
    end
    prompt, phase = nil, "stopping"
    clear_output()
    -- The RPC owns the runtime step while suspended, so drain the child here.
    while child and phase == "stopping" do
      ctx.wait()
      step()
    end
    return { phase = phase, pendingMessageID = pending or ctx.json.null }
  end
  phase = "closed"
  return { phase = phase, pendingMessageID = ctx.json.null }
end

local function poll(params)
  local limit = params.maxEvents
  if limit == nil then
    return table.remove(events, 1) or { source = "none", phase = phase }
  end
  if math.type(limit) ~= "integer" or limit < 1 or limit > 32 then
    return nil, "invalid"
  end
  local batch = ctx.json.array()
  -- Each event is read through the response receipt before the next event.
  if events[1] then
    batch[1] = table.remove(events, 1)
  end
  return { source = "batch", events = batch, phase = phase }
end

handles = { start = start, send = send, interrupt = interrupt, stop = stop, poll = poll }

local function refusal(status)
  return { status = status, responseID = "", totalUnits = 0, totalBytes = 0 }
end

local function call(params)
  local method = handles[params.method]
  if not method then
    return refusal("unsupported")
  end
  if not input or not input.sealed or input.id ~= params.inputID then
    return refusal("invalid")
  end
  if receipt_count >= 4 then
    return refusal("limit")
  end
  local data, id = input.data, input.id
  input = nil
  local value = ctx.json.decode(data)
  if type(value) ~= "table" then
    return refusal("invalid")
  end
  local answer, status = method(value)
  if not answer then
    return refusal(status)
  end
  local response_id = identifier("response-")
  command_sequence = command_sequence + 1
  local encoded = ctx.json.encode({ version = 1, id = "command-" .. command_sequence,
                                    sequence = command_sequence,
                                    ok = true, result = answer }, receipt_limit)
  if not encoded then
    fail("RESPONSE_LIMIT")
    return refusal("limit")
  end
  receipts[response_id] = {data = encoded, units = units(encoded)}
  receipt_count = receipt_count + 1
  latest_response, latest_input = response_id, id
  return { status = "ok", responseID = response_id, totalUnits = units(encoded), totalBytes = #encoded }
end

local function open_input(params)
  local total = params.totalUnits
  if input or math.type(total) ~= "integer" or total < 2 or total > 3072 then
    return {status = input and "busy" or "invalid", inputID = "", nextOffset = 0}
  end
  input = {id = identifier("input-"), total = total, data = "", length = 0}
  return {status = "ok", inputID = input.id, nextOffset = 0}
end

local function append_input(params)
  local content = params.content
  local length = type(content) == "string" and units(content)
  local offset = math.type(params.offset) == "integer" and params.offset or 0
  if not input or input.id ~= params.inputID or input.sealed or offset ~= input.length
      or not length or length < 1 or length > 256 or input.length + length > input.total
      or #input.data + #content > 3072 then
    return {status = "invalid", nextOffset = offset}
  end
  input.data = input.data .. content
  input.length = input.length + length
  return {status = "ok", nextOffset = input.length}
end

local function seal_input(params)
  if not input or input.id ~= params.inputID or input.length ~= input.total then
    return {status = "invalid"}
  end
  input.sealed = true
  return {status = "ok"}
end

local function discard_input(params)
  local accepted = input and input.id == params.inputID or false
  if accepted then input = nil end
  return {accepted = accepted}
end

local function read_receipt(params)
  local id = type(params.responseID) == "string" and params.responseID or ""
  local receipt = receipts[id]
  local offset = math.type(params.offset) == "integer" and params.offset or 0
  local result = {status = "missing", responseID = id, offset = offset, nextOffset = offset,
                  totalUnits = 0, totalBytes = 0, content = "", done = false}
  if receipt then
    result.totalUnits, result.totalBytes = receipt.units, #receipt.data
    local data, next_offset = next_page(receipt.data, offset)
    result.status = data and "ok" or "invalid"
    if data then
      result.content, result.nextOffset, result.done = data, next_offset, next_offset == receipt.units
    end
  end
  return result
end

local function forget_receipt(params)
  local id = params.responseID
  if type(id) ~= "string" or not receipts[id] then
    return {accepted = false}
  end
  receipts[id] = nil
  receipt_count = receipt_count - 1
  if latest_response == id then latest_response, latest_input = "", "" end
  return {accepted = true}
end

ctx.onStep(step)
return {
  ["claude.input.open"] = open_input,
  ["claude.input.append"] = append_input,
  ["claude.input.seal"] = seal_input,
  ["claude.input.discard"] = discard_input,
  ["claude.call"] = call,
  ["claude.response.read"] = read_receipt,
  ["claude.response.forget"] = forget_receipt,
  ["claude.status"] = function()
    return {phase = phase, failure = failure, exitJSON = ctx.json.encode(exit_info),
            reaped = reaped, responseID = latest_response, inputID = latest_input}
  end,
}
