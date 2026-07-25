#!/usr/bin/env node
/**
 * Vortek client/server source + staged ICD verification (Task 2/3 gates).
 */
import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

const APPROVED_API_VERSION = "1.3.0";
const CLIENT_NAME = "vortek-client";
const SERVER_NAME = "vortek-server";

function fail(message) {
  throw new Error(message);
}

function sha256File(path) {
  return createHash("sha256").update(readFileSync(path)).digest("hex");
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "../..");
const lockPath = resolve(projectRoot, "runtime/locks/components.lock.json");
const protocolManifestPath = resolve(projectRoot, "runtime/locks/vortek-protocol.sha256");
const noticePath = resolve(projectRoot, "NOTICE.android-runtime.md");
const licensePath = resolve(projectRoot, "LICENSES/Vortek-LGPL-2.1.txt");
const stageDir = resolve(projectRoot, "runtime/build/vortek-client-stage");
const clientLib = resolve(stageDir, "host/lib/libvulkan_vortek.so");
const icdPath = resolve(stageDir, "host/vulkan/icd.d/vortek.json");
const protocolHeader = resolve(projectRoot, "runtime/vortek-protocol/bachata_vortek_protocol.h");

const lock = JSON.parse(readFileSync(lockPath, "utf8"));
const client = lock.components.find((c) => c.name === CLIENT_NAME);
const server = lock.components.find((c) => c.name === SERVER_NAME);
const winlator = lock.components.find((c) => c.name === "winlator-app");
if (!client) fail("lock missing vortek-client");
if (!server) fail("lock missing vortek-server");
if (!winlator) fail("lock missing winlator-app");
if (!/^[0-9a-f]{40}$/.test(client.revision)) fail("vortek-client revision must be 40 hex chars");
if (!/^[0-9a-f]{40}$/.test(server.revision)) fail("vortek-server revision must be 40 hex chars");
if (server.revision !== winlator.revision) fail("vortek-server revision must equal winlator-app");
if (client.url !== "https://github.com/brunodev85/vortek.git") fail("unexpected vortek-client url");
if (client.license !== "LGPL-2.1") fail("vortek-client license must be LGPL-2.1");
if (server.license !== "LGPL-2.1") fail("vortek-server license must be LGPL-2.1");
if (client.sourceDestination !== "runtime/sources/vortek-client") fail("bad client sourceDestination");
if (server.sourceDestination !== "runtime/sources/winlator-app/app/src/main/cpp/vortekrenderer") {
  fail("bad server sourceDestination");
}
if (client.buildOutput !== "host/lib/libvulkan_vortek.so") fail("bad client buildOutput");

const clientSource = resolve(projectRoot, client.sourceDestination);
const serverSource = resolve(projectRoot, server.sourceDestination);
if (!existsSync(clientSource)) fail(`missing client source: ${clientSource}`);
if (!existsSync(serverSource)) fail(`missing server source: ${serverSource}`);
if (!existsSync(resolve(clientSource, "LICENSE"))) fail("missing client LICENSE");
if (!existsSync(licensePath)) fail("missing LICENSES/Vortek-LGPL-2.1.txt — run vendor-vortek.sh");
if (!existsSync(protocolHeader)) fail("missing shared protocol header");

const clientHead = execFileSync("git", ["-C", clientSource, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (clientHead !== client.revision) fail(`client HEAD ${clientHead} != lock ${client.revision}`);

const winlatorRoot = resolve(projectRoot, "runtime/sources/winlator-app");
if (existsSync(resolve(winlatorRoot, ".git"))) {
  const serverHead = execFileSync("git", ["-C", winlatorRoot, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
  if (serverHead !== server.revision) fail(`winlator-app HEAD ${serverHead} != lock ${server.revision}`);
}

const clientReq = resolve(clientSource, "include/request_codes.h");
const serverReq = resolve(serverSource, "include/request_codes.h");
const clientSer = resolve(clientSource, "include/vortek_serializer.h");
const serverSer = resolve(serverSource, "include/vortek_serializer.h");
const reqClient = sha256File(clientReq);
const reqServer = sha256File(serverReq);
const serClient = sha256File(clientSer);
const serServer = sha256File(serverSer);
if (reqClient !== reqServer) {
  fail("Vortek protocol mismatch: client request_codes.h does not match pinned Winlator server.");
}
if (serClient !== serServer) {
  fail("Vortek protocol mismatch: client vortek_serializer.h does not match pinned Winlator server.");
}
if (!existsSync(protocolManifestPath)) fail("missing runtime/locks/vortek-protocol.sha256");

const notice = readFileSync(noticePath, "utf8");
for (const needle of [
  "brunodev85/vortek",
  client.revision,
  "LGPL-2.1",
  "vortekrenderer",
  server.revision,
]) {
  if (!notice.includes(needle)) fail(`NOTICE.android-runtime.md missing: ${needle}`);
}

if (!existsSync(clientLib)) fail(`missing staged client library: ${clientLib}`);
if (!existsSync(icdPath)) fail(`missing staged ICD: ${icdPath}`);

const icd = JSON.parse(readFileSync(icdPath, "utf8"));
if (icd.ICD?.api_version !== APPROVED_API_VERSION) {
  fail(`ICD api_version must remain ${APPROVED_API_VERSION}, got ${icd.ICD?.api_version}`);
}
// Task 8: truthful 1.3.0 only — reject accidental 1.4+ over-advertisement.
if (/^1\.(4|5|6)\./.test(String(icd.ICD?.api_version || ""))) {
  fail(`ICD must not over-advertise beyond approved ${APPROVED_API_VERSION}`);
}
const libraryPath = String(icd.ICD?.library_path || "");
if (!libraryPath) fail("ICD missing library_path");
if (libraryPath.includes("com.winlator")) fail("ICD library_path contains Winlator path");
if (libraryPath.includes("/data/data/com.winlator")) fail("ICD library_path hardcodes Winlator package");
if (!libraryPath.includes("libvulkan_vortek.so")) fail("ICD library_path must reference libvulkan_vortek.so");

const fileOut = execFileSync("file", ["-b", clientLib], { encoding: "utf8" });
if (!/aarch64|ARM aarch64/i.test(fileOut)) fail(`client library not aarch64: ${fileOut}`);
if (/android/i.test(fileOut)) fail(`client library looks Android/Bionic: ${fileOut}`);

const dynamic = execFileSync("readelf", ["-d", clientLib], { encoding: "utf8" });
if (!dynamic.includes("libc.so.6")) fail("client library does not link glibc libc.so.6");
if (/linker64|libandroid|bionic/i.test(dynamic)) fail("client library appears to use Android linker");

const symbols = execFileSync("readelf", ["-Ws", clientLib], { encoding: "utf8" });
for (const sym of ["vk_icdGetInstanceProcAddr", "vk_icdNegotiateLoaderICDInterfaceVersion"]) {
  if (!symbols.includes(sym)) fail(`missing export ${sym}`);
}

const sourceMeta = readFileSync(resolve(stageDir, "usr/share/bachata/vortek/SOURCE.txt"), "utf8");
if (!sourceMeta.includes(client.revision)) fail("SOURCE.txt missing client revision");
if (!sourceMeta.includes(server.revision)) fail("SOURCE.txt missing server revision");
if (!sourceMeta.includes("built_from_source=true")) fail("SOURCE.txt must state built_from_source=true");

console.log(
  `vortek verified: client=${client.revision} server=${server.revision} protocol_match=true api=${APPROVED_API_VERSION}`,
);
