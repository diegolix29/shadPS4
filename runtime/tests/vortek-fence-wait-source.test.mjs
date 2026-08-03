import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("Vortek WaitForFences handles create-signaled SYNC_FD without false DEVICE_LOST", () => {
  const server = read(
    "runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer/src/request_handler.c",
  );
  const client = read("runtime/sources/vortek-client/src/vulkan_calls.c");
  const clientSock = read("runtime/sources/vortek-client/include/socket_utils.h");

  const serverWait = server.slice(server.indexOf("void vt_handle_vkWaitForFences"));
  // Must filter Android already-signaled fd=-1 and never SCM_RIGHTS them.
  assert.match(serverWait, /fd >= 0/);
  assert.match(serverWait, /validFdCount/);
  // GetFenceFd failure falls back to host wait + 0-FD reply.
  assert.match(
    serverWait,
    /vkGetFenceFd[\s\S]*vkWaitForFences[\s\S]*send_fds\(context->clientFd, NULL, 0/,
  );

  const clientWait = client.slice(client.indexOf("vt_call_vkWaitForFences"));
  // SUCCESS + 0 FDs is valid (host waited / already signaled), not DEVICE_LOST.
  assert.match(clientWait, /if \(numFds == 0\) return VK_SUCCESS/);
  assert.doesNotMatch(
    clientWait,
    /if \(numFds == 0 \|\| result != VK_SUCCESS\) return VK_ERROR_DEVICE_LOST/,
  );

  // Client send_fds must allow 0-FD data-only replies.
  assert.match(clientSock, /msg_control = NULL/);
  assert.match(clientSock, /numFds > 0 && fds/);
});
