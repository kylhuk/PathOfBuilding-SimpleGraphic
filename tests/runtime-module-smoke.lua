-- This script is intentionally GUI-free.  It verifies that a staged runtime
-- resolves every bundled Lua module through its relocatable package paths.
local modules = {
  "lua-utf8",
  "socket",
  "socket.core",
  "mime",
  "mime.core",
  "lzip",
  "lcurl",
  "lcurl.safe",
  "cURL",
  "cURL.safe",
}

for _, module in ipairs(modules) do
  assert(require(module), "failed to load " .. module)
end

if package.config:sub(1, 1) == "/" then
  assert(require("socket.unix"), "failed to load socket.unix")
end

Exit()
