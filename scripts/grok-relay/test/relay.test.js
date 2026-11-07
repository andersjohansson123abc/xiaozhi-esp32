// Runs the Worker against real SQLite (sql.js) and a stubbed webhook. Open
// test/run.html through test/serve.ps1; the page reports PASS or the failures.
import worker, { effectiveState } from "../src/index.js";

const out = document.getElementById("out");
const failures = [];
let checks = 0;
function check(condition, label) {
  checks++;
  if (!condition) failures.push(label);
}

// The part of the D1 API the Worker uses.
function d1(database) {
  const prepare = (sql, params = []) => ({
    bind: (...values) => prepare(sql, values),
    all() {
      const statement = database.prepare(sql);
      statement.bind(params);
      const results = [];
      while (statement.step()) results.push(statement.getAsObject());
      statement.free();
      return Promise.resolve({ results });
    },
    async first() {
      return (await this.all()).results[0] ?? null;
    },
    run() {
      database.run(sql, params);
      return Promise.resolve({ success: true });
    },
  });
  return { prepare };
}

const SQL = await initSqlJs({
  locateFile: (file) => `https://cdnjs.cloudflare.com/ajax/libs/sql.js/1.10.3/${file}`,
});
const realFetch = globalThis.fetch;
const schema = await (await realFetch("../schema.sql")).text();

function freshEnv(overrides = {}) {
  const database = new SQL.Database();
  database.run(schema);
  return {
    DB: d1(database),
    DEVICE_TOKEN: "device-token",
    BOT_TOKEN: "bot-token",
    BOTS: JSON.stringify([
      { id: "salj", name: "Säljboten", webhook: "https://hook.test/salj", key: "k1" },
      { id: "drift", name: "Drift", webhook: "https://hook.test/drift", key: "k2" },
    ]),
    ...overrides,
  };
}

let hooks = [];
let hookStatus = 200;
function stubHooks() {
  hooks = [];
  hookStatus = 200;
  globalThis.fetch = async (url, init) => {
    hooks.push({ url, init });
    if (hookStatus === 0) throw new TypeError("network");
    return new Response("", { status: hookStatus });
  };
}

const call = (env, path, init = {}) =>
  worker.fetch(new Request(`https://relay.test${path}`, init), env);
const device = (env, path, init = {}) =>
  call(env, path, { ...init, headers: { authorization: "Bearer device-token", ...init.headers } });
const poll = async (env, query = "") => (await device(env, `/device/poll${query}`)).text();
const send = (env, bot, text) =>
  device(env, "/device/send", {
    method: "POST",
    headers: { "content-type": "application/x-www-form-urlencoded" },
    body: new URLSearchParams({ bot, text }).toString(),
  });
let rpcId = 0;
async function mcp(env, method, params, path = "/mcp/bot-token") {
  const response = await call(env, path, {
    method: "POST",
    body: JSON.stringify({ jsonrpc: "2.0", id: ++rpcId, method, params }),
  });
  return response.json();
}
const tool = (env, name, args) => mcp(env, "tools/call", { name, arguments: args });
const lines = (body, kind) => body.split("\n").filter((line) => line.startsWith(`${kind}\t`));

async function run() {
  stubHooks();

  // Auth.
  let env = freshEnv();
  check((await call(env, "/device/poll")).status === 401, "poll without token");
  check(
    (await call(env, "/device/poll", { headers: { authorization: "Bearer bot-token" } })).status ===
      401,
    "bot token must not open the device route",
  );
  check((await call(env, "/mcp/device-token", { method: "POST", body: "{}" })).status === 401,
    "device token must not open MCP");
  check((await call(env, "/mcp", { method: "POST", body: "{}" })).status === 401, "mcp no token");
  const unset = freshEnv({ BOT_TOKEN: "" });
  check((await call(unset, "/mcp/", { method: "POST", body: "{}" })).status === 401,
    "an unset token must never match");
  check((await call(env, "/nope")).status === 404, "unknown path");
  check((await device(env, "/device/poll?ack=x")).status === 400, "bad ack");

  // Poll shape before any Bot has reported.
  let body = await poll(env);
  check(body.startsWith("GROK1\nU\t0\n"), "header");
  check(lines(body, "B")[0] === "B\tsalj\tSäljboten\tunknown\t-1\t", "unreported bot");

  // MCP handshake, over both auth forms.
  const init = await mcp(env, "initialize", { protocolVersion: "2025-03-26" });
  check(init.result.protocolVersion === "2025-03-26", "echoes protocol version");
  const listed = await mcp(env, "tools/list", {});
  check(listed.result.tools.length === 2, "two tools");
  const viaHeader = await call(env, "/mcp", {
    method: "POST",
    headers: { authorization: "Bearer bot-token" },
    body: JSON.stringify({ jsonrpc: "2.0", id: 9, method: "ping" }),
  });
  check((await viaHeader.json()).result !== undefined, "bearer auth on /mcp");
  const notified = await call(env, "/mcp/bot-token", {
    method: "POST",
    body: JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }),
  });
  check(notified.status === 202, "notification gets 202");
  check((await mcp(env, "nonsense")).error.code === -32601, "unknown method");

  // Status report, with hostile text.
  let result = await tool(env, "passport_report_status", {
    bot_id: "salj",
    state: "working",
    note: "Tab\there\nline 🚀 " + "x".repeat(90),
  });
  check(!result.result.isError, "status accepted");
  body = await poll(env);
  const row = lines(body, "B")[0].split("\t");
  check(row.length === 6, "a note cannot add a field");
  check(row[3] === "working", "reported state");
  check(row[5].startsWith("Tab here line") && !row[5].includes("🚀"), "note is cleaned");
  check(Array.from(row[5]).length <= 48, "note is capped");
  result = await tool(env, "passport_report_status", { bot_id: "salj", state: "banana" });
  check(result.result.isError, "bad state refused");
  result = await tool(env, "passport_reply", { bot_id: "ghost", text: "hej" });
  check(result.result.isError && result.result.content[0].text.includes("salj"), "unknown bot");

  // Device to Bot.
  const response = await send(env, "salj", "Hur går offerten åt Kalle?");
  check(response.status === 200, "send accepted");
  check(hooks.length === 1 && hooks[0].url === "https://hook.test/salj", "right webhook");
  check(hooks[0].init.headers.authorization === "Bearer k1", "webhook key");
  check(JSON.parse(hooks[0].init.body).message === "Hur går offerten åt Kalle?", "webhook body");
  check(!body.includes("k1") && !body.includes("hook.test"), "webhook stays on the relay");
  check((await send(env, "ghost", "hej")).status === 404, "unknown bot on send");
  check((await send(env, "salj", "   ")).status === 400, "empty message");

  // Bot to device, then the ack.
  result = await tool(env, "passport_reply", { bot_id: "salj", text: "Offerten är klar.\nSkickad." });
  check(result.result.content[0].text === "Delivered.", "reply delivered");
  body = await poll(env);
  check(body.includes("U\t1\n"), "one unread");
  const message = lines(body, "M")[0].split("\t");
  check(message[2] === "salj" && message[4] === "Offerten är klar. Skickad.", "message line");
  check(lines(body, "B")[0].split("\t")[3] === "idle", "a reply ends the run");
  body = await poll(env, `?ack=${message[1]}`);
  check(body.includes("U\t0\n") && lines(body, "M").length === 0, "ack clears unread");

  // A long reply is cut and the Bot is told.
  const long = await tool(env, "passport_reply", { bot_id: "drift", text: "å".repeat(500) });
  check(long.result.content[0].text.includes("cut"), "truncation reported");
  body = await poll(env);
  check(Array.from(lines(body, "M")[0].split("\t")[4]).length === 400, "reply capped");

  // At most three messages per poll, oldest first, and the count stays honest.
  env = freshEnv();
  for (let i = 1; i <= 5; i++) await tool(env, "passport_reply", { bot_id: "salj", text: `s${i}` });
  body = await poll(env);
  check(body.includes("U\t5\n") && lines(body, "M").length === 3, "poll is capped");
  check(lines(body, "M")[0].endsWith("\ts1"), "oldest first");

  // A refused or unreachable webhook is visible, and the next good send clears it.
  env = freshEnv();
  hookStatus = 401;
  check((await send(env, "drift", "hej")).status === 502, "refused webhook is a 502");
  check(lines(await poll(env), "B")[1].split("\t")[3] === "error", "refusal shows on the bot");
  hookStatus = 0;
  check((await send(env, "drift", "hej")).status === 502, "network failure is a 502");
  hookStatus = 200;
  check((await send(env, "drift", "hej")).status === 200, "recovers");
  check(lines(await poll(env), "B")[1].split("\t")[3] === "unknown", "error cleared");

  // Rate limit.
  env = freshEnv({ MAX_SENDS_PER_HOUR: "2" });
  await send(env, "salj", "1");
  await send(env, "salj", "2");
  hooks = [];
  check((await send(env, "salj", "3")).status === 429, "rate limited");
  check(hooks.length === 0, "a limited send reaches no webhook");

  // Bad configuration fails closed and leaks nothing.
  env = freshEnv({ BOTS: "{oops" });
  const broken = await device(env, "/device/poll");
  check(broken.status === 500 && (await broken.text()) === "relay error", "bad BOTS");
  env = freshEnv({
    BOTS: JSON.stringify([{ id: "a", name: "A", webhook: "http://x", key: "k" }]),
  });
  check((await device(env, "/device/poll")).status === 500, "plain http webhook refused");

  // Derived states.
  const cfg = { NO_REPLY_SECONDS: "900", STALE_SECONDS: "7200" };
  const at = 100000;
  check(effectiveState(undefined, at, cfg).state === "unknown", "no row");
  check(
    effectiveState({ sent_at: at - 899, reported_at: at, state: "idle" }, at, cfg).state === "idle",
    "still inside the reply window",
  );
  check(effectiveState({ sent_at: at - 900 }, at, cfg).state === "noreply", "no reply");
  check(
    effectiveState(
      { sent_at: at - 900, replied_at: at - 100, reported_at: at, state: "idle" },
      at,
      cfg,
    ).state === "idle",
    "answered",
  );
  check(effectiveState({ reported_at: at - 7200, state: "working" }, at, cfg).state === "quiet",
    "stuck working");
  check(effectiveState({ reported_at: at - 90000, state: "idle" }, at, cfg).state === "idle",
    "a long idle is normal");
}

try {
  await run();
  out.textContent = failures.length
    ? `FAIL ${failures.length} of ${checks}\n${failures.join("\n")}`
    : `PASS ${checks} checks`;
} catch (error) {
  out.textContent = `CRASH ${error.stack}\nfailures so far: ${failures.join("; ")}`;
} finally {
  globalThis.fetch = realFetch;
}
